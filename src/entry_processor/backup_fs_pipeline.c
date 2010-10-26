/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=4:tabstop=4:
 */
/*
 * Copyright (C) 2009, 2010 CEA/DAM
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the CeCILL License.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license (http://www.cecill.info) and that you
 * accept its terms.
 */
/**
 * Pipeline functions for FS backup purpose
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "RobinhoodConfig.h"
#include "RobinhoodLogs.h"
#include "RobinhoodMisc.h"
#include "entry_processor.h"
#include "entry_proc_tools.h"
#include <errno.h>
#include <time.h>

#define ERR_MISSING(_err) (((_err)==ENOENT)||((_err)==ESTALE))

/* declaration of EntryProc functions of pipeline */
int            EntryProc_get_fid( struct entry_proc_op_t *, lmgr_t * );
int            EntryProc_get_info_db( struct entry_proc_op_t *, lmgr_t * );
int            EntryProc_get_info_fs( struct entry_proc_op_t *, lmgr_t * );
int            EntryProc_reporting( struct entry_proc_op_t *, lmgr_t * );
int            EntryProc_db_apply( struct entry_proc_op_t *, lmgr_t * );
#ifdef HAVE_CHANGELOGS
int            EntryProc_chglog_clr( struct entry_proc_op_t *, lmgr_t * );
#endif
int            EntryProc_rm_old_entries( struct entry_proc_op_t *, lmgr_t * );

/** pipeline stages definition */
pipeline_stage_t entry_proc_pipeline[] = {
    {STAGE_GET_FID, "STAGE_GET_FID", EntryProc_get_fid,
     STAGE_FLAG_PARALLEL | STAGE_FLAG_SYNC, 0},
    {STAGE_GET_INFO_DB, "STAGE_GET_INFO_DB", EntryProc_get_info_db,
     STAGE_FLAG_PARALLEL | STAGE_FLAG_SYNC | STAGE_FLAG_ID_CONSTRAINT, 0},
    {STAGE_GET_INFO_FS, "STAGE_GET_INFO_FS", EntryProc_get_info_fs,
     STAGE_FLAG_PARALLEL | STAGE_FLAG_SYNC, 0},
    {STAGE_REPORTING, "STAGE_REPORTING", EntryProc_reporting,
     STAGE_FLAG_PARALLEL | STAGE_FLAG_ASYNC, 0},
    {STAGE_DB_APPLY, "STAGE_DB_APPLY", EntryProc_db_apply,
#if defined( _SQLITE )
     /* SQLite locks the whole file for modifications...
     * So, 1 single threads is enough at this step.
     */
     STAGE_FLAG_MAX_THREADS | STAGE_FLAG_SYNC, 1},
#else
     STAGE_FLAG_PARALLEL | STAGE_FLAG_SYNC, 0},
#endif

#ifdef HAVE_CHANGELOGS
    /* only 1 thread here because commiting records must be sequential
     * (in the same order as changelog) */
    {STAGE_CHGLOG_CLR, "STAGE_CHGLOG_CLR", EntryProc_chglog_clr,
     STAGE_FLAG_SEQUENTIAL | STAGE_FLAG_SYNC, 1},

     /* acknowledging records must be sequential,
      * in the order of record ids
      * @TODO change this depending on the mode the program is started.
      */
#endif
    /* this step is for mass update / mass remove operations when
     * starting/ending a FS scan. */
    {STAGE_RM_OLD_ENTRIES, "STAGE_RM_OLD_ENTRIES", EntryProc_rm_old_entries,
     STAGE_FLAG_SEQUENTIAL | STAGE_FLAG_SYNC, 0}
};


/**
 * For entries from FS scan, we must get the associated entry ID.
 */
int EntryProc_get_fid( struct entry_proc_op_t *p_op, lmgr_t * lmgr )
{
#ifdef _HAVE_FID
    int            rc;
    entry_id_t     tmp_id;

    if ( !p_op->entry_attr_is_set
         || !ATTR_MASK_TEST( &p_op->entry_attr, fullpath ) )
    {
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                    "Error: entry full path is expected to be set"
                    " in STAGE_GET_FID stage" );
        EntryProcessor_Acknowledge( p_op, 0, TRUE );
        return EINVAL;
    }

    /* perform path2fid */
    rc = Lustre_GetFidFromPath( ATTR( &p_op->entry_attr, fullpath ), &tmp_id );
    if ( rc )
    {
        /* remove the operation from pipeline */
        rc = EntryProcessor_Acknowledge( p_op, 0, TRUE );
        if ( rc )
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                        "Error %d acknowledging stage STAGE_GET_FID.", rc );
    }
    else
    {
        rc = EntryProcessor_SetEntryId( p_op, &tmp_id );
        if ( rc )
        {
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d setting entry id",
                        rc );
            /* remove entry from pipeline */
            EntryProcessor_Acknowledge( p_op, 0, TRUE );
            return rc;
        }

        /* go to GET_INFO_DB stage */
        rc = EntryProcessor_Acknowledge( p_op, STAGE_GET_INFO_DB, FALSE );
        if ( rc )
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                        "Error %d acknowledging stage STAGE_GET_FID.", rc );
    }
    return rc;
#else
    DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error: unexpected stage in a filesystem with no fid: STAGE_GET_FID.");
    EntryProcessor_Acknowledge( p_op, 0, TRUE );
    return EINVAL;
#endif
}


#ifdef HAVE_CHANGELOGS
/**
 * Infer information from the changelog record (status, ...).
 * \return next pipeline step to be perfomed.
 */
static int EntryProc_FillFromLogRec( struct entry_proc_op_t *p_op,
                                     int allow_md_updt, int allow_path_updt )
{
    /* alias to the log record */
    struct changelog_rec * logrec = p_op->extra_info.log_record.p_log_rec;

    /* if this is a CREATE record, we know that its status is NEW. */
    if ( logrec->cr_type == CL_CREATE )
    {
        /* Sanity check: if the entry already exists in DB,
         * it could come from a previous filesystem that has been formatted.
         * In this case, force a full update of the entry.
         */
        if ( p_op->db_exists )
        {
            if ( p_op->entry_attr_is_set
                 && ATTR_MASK_TEST( &p_op->entry_attr, fullpath ) )
                DisplayLog( LVL_MAJOR, ENTRYPROC_TAG,
                            "WARNING: CREATE record on already existing entry fid="
                            DFID", path=%s", PFID(&p_op->entry_id),
                            ATTR( &p_op->entry_attr, fullpath ) );
            else
                DisplayLog( LVL_MAJOR, ENTRYPROC_TAG,
                            "WARNING: CREATE record on already existing entry fid="
                            DFID, PFID(&p_op->entry_id) );

            /* set insertion time, like for a new entry */
            p_op->entry_attr_is_set = TRUE;
            ATTR_MASK_SET( &p_op->entry_attr, creation_time );
            ATTR( &p_op->entry_attr, creation_time )
                = cltime2sec(logrec->cr_time);

            p_op->extra_info_is_set = TRUE;
            p_op->extra_info.getattr_needed = TRUE;
            p_op->extra_info.getpath_needed = TRUE;
            p_op->extra_info.getstripe_needed = TRUE;
            p_op->extra_info.getstatus_needed = TRUE;
        }
        else /* new entry */
        {
            p_op->entry_attr_is_set = TRUE;
            ATTR_MASK_SET( &p_op->entry_attr, status );
            ATTR( &p_op->entry_attr, status ) = STATUS_NEW;

            /* no flag is set for now */
            ATTR_MASK_SET( &p_op->entry_attr, no_archive );
            ATTR( &p_op->entry_attr, no_archive ) = 0;
            p_op->extra_info.getstatus_needed = FALSE;

            /* new entry: never archived or restored */
            ATTR( &p_op->entry_attr, last_archive ) = 0;
            ATTR_MASK_SET( &p_op->entry_attr, last_archive );
        }
    }
#ifndef CLF_UNLINK_LAST
    /* if the log record does not indicate if the entry still exists,
     * force performing "lstat()" after UNLINK, to verify if the entry
     * still exists.  Also need to get status to test if there is an
     * orphan entry in the backend.
     */
    else if (logrec->cr_type == CL_UNLINK )
    {
        p_op->extra_info_is_set = TRUE;
        p_op->extra_info.getattr_needed = TRUE;
        if ( !policies.unlink_policy.no_hsm_remove )
            p_op->extra_info.getstatus_needed = TRUE;
    }
#endif

    /* if the entry is already in DB, try to determine if something changed */
    if ( p_op->db_exists )
    {
        /* compare to the name in changelog */
        if ( allow_path_updt && (logrec->cr_namelen > 0) )
        {
            /* if name is set, compare to name */
            if ( ATTR_MASK_TEST( &p_op->entry_attr, name ) )
            {
                if ( strcmp(logrec->cr_name, ATTR( &p_op->entry_attr, name )) )
                {
                    p_op->extra_info.getpath_needed = TRUE;
                    DisplayLog( LVL_DEBUG, ENTRYPROC_TAG,
                             "Getpath needed because name changed: '%s'->'%s'",
                             ATTR( &p_op->entry_attr, name ), logrec->cr_name );
                }
            }
            else if ( ATTR_MASK_TEST( &p_op->entry_attr, fullpath ) )
            {
                /* else, compare using full path */
                char * basename;
                basename = strrchr( ATTR( &p_op->entry_attr, fullpath ), '/' );
                 if ( basename )
                 {
                    basename++;
                    if ( strcmp( basename, logrec->cr_name ) )
                    {
                        p_op->extra_info.getpath_needed = TRUE;
                        DisplayLog( LVL_DEBUG, ENTRYPROC_TAG,
                                    "Getpath needed because name from log record "
                                    "(%s) doesn't match fullpath '%s'",
                                    logrec->cr_name ,
                                    ATTR( &p_op->entry_attr, fullpath ) );
                    }
                 }
            }
        }

        /* in case of a rename, refresh the full path (after rename => rnm to only CL_EXT)*/
        if ( allow_path_updt && (logrec->cr_type == CL_EXT) )
        {
            /* file was renamed */
            DisplayLog( LVL_DEBUG, ENTRYPROC_TAG, "Getpath needed because it's "
                        "a rename operation" );
            p_op->extra_info.getpath_needed = TRUE;
        }

        /* get the new attributes, in case of a SATTR, xTIME, HSM... */
        if ( allow_md_updt && (CL_TIME_NOACCESS(logrec->cr_type)
                               || ( logrec->cr_type == CL_TRUNC )
                               || ( logrec->cr_type == CL_HSM )
                               || ( logrec->cr_type == CL_SETATTR )) )
        {
            p_op->extra_info.getattr_needed = TRUE;
        }
    }

    /* other records: keep default value for getstatus_needed */

    return STAGE_GET_INFO_FS;
}


/**
 *  Infer information and determine needed information from a changelog record.
 *  \return the next pipeline step to be performed,
 *          -1 if entry must be dropped.
 */
static int EntryProc_ProcessLogRec( struct entry_proc_op_t *p_op )
{
    /* short alias */
    struct changelog_rec * logrec = p_op->extra_info.log_record.p_log_rec;

    /* allow event-driven update */
    int md_allow_event_updt = TRUE;
    int path_allow_event_updt = TRUE;

    if ( logrec->cr_type == CL_UNLINK )
    {
        DisplayLog( LVL_DEBUG, ENTRYPROC_TAG,
#ifdef CLF_UNLINK_LAST
                    "UNLINK on %s entry "DFID": last=%s",
                    p_op->db_exists?"known":"unknown", PFID(&p_op->entry_id),
                    bool2str( logrec->cr_flags & CLF_UNLINK_LAST ) );
#else
                    "UNLINK on %s entry "DFID,
                    p_op->db_exists?"known":"unknown", PFID(&p_op->entry_id));
#endif

#ifdef CLF_UNLINK_LAST
        /* it it the last reference to this file? */
        if ( logrec->cr_flags & CLF_UNLINK_LAST )
        {
            if ( policies.unlink_policy.no_hsm_remove )
            {
                /*  hsm_remove is disabled or file doesn't exist in the HSM:
                 * If the file was in DB: remove it, else skip the record. */
                if ( p_op->db_exists )
                {
                    p_op->db_op_type = OP_TYPE_REMOVE;
                    return STAGE_DB_APPLY;
                }
                else
                    return STAGE_CHGLOG_CLR;
            }
            else /* hsm removal enabled: must check if there is some cleaning
                  * to be done in the backend */
            {
                /* If the entry exists in DB, this moves it from main table
                 * to a remove queue, else, just insert it to remove queue. */
                p_op->db_op_type = OP_TYPE_SOFT_REMOVE;
                return STAGE_DB_APPLY;
            }
        }
        else
#endif
        if ( p_op->db_exists ) /* entry still exists and is known in DB */
        {
            /* Force updating the path, because the path we have may be
             * the removed one. */
            p_op->extra_info_is_set = TRUE;
            p_op->extra_info.getpath_needed = TRUE;
            /* then, check needed updates in the next part of the function */
        }
        /* else: is handled in the next part of the function */

    } /* end if UNLINK */

    if ( !p_op->db_exists )
    {
        /* non-unlink (or non-destructive unlink) record on unknown entry:
         * insert entry to the DB */
        p_op->db_op_type = OP_TYPE_INSERT;

        /* new entry, set insertion time */
        p_op->entry_attr_is_set = TRUE;
        ATTR_MASK_SET( &p_op->entry_attr, creation_time );
        ATTR( &p_op->entry_attr, creation_time ) = cltime2sec(logrec->cr_time);

        /* we must get info that is not provided by the chglog */
        p_op->extra_info_is_set = TRUE;
        p_op->extra_info.getattr_needed = TRUE;
        p_op->extra_info.getpath_needed = TRUE;
        p_op->extra_info.getstripe_needed = TRUE;
        /* By default, get status for a new record.
         * This is overwritten by EntryProc_FillFromLogRec if the status
         * can be infered from changelog rec. */
        p_op->extra_info.getstatus_needed = TRUE;
    }
    else /* non-unlink record on known entry */
    {
        p_op->db_op_type = OP_TYPE_UPDATE;

        /* check what information must be updated */

        if ( !p_op->entry_attr_is_set )
        {
            /* we know nothing about it... */
            p_op->extra_info_is_set = TRUE;
            p_op->extra_info.getattr_needed = TRUE;
            p_op->extra_info.getpath_needed = TRUE;
            p_op->extra_info.getstripe_needed = TRUE;
            p_op->extra_info.getstatus_needed = TRUE;
        }
        else
        {
            p_op->extra_info_is_set = TRUE;

            /* get stripe info only if we don't already know it. */
            p_op->extra_info.getstripe_needed =
                ATTR_MASK_TEST( &p_op->entry_attr, stripe_info )? FALSE : TRUE;

            /* get HSM status only if it is not already in the DB. */
            p_op->extra_info.getstatus_needed =
                ATTR_MASK_TEST( &p_op->entry_attr, status )? FALSE : TRUE;

            /* Check md_update policy */
            p_op->extra_info.getattr_needed
                = need_md_update( &p_op->entry_attr, &md_allow_event_updt );

            /* check if path update is needed */
            p_op->extra_info.getpath_needed
                = need_path_update( &p_op->entry_attr, &path_allow_event_updt );
        }
    }

    /* infer info from changelog record, then continue to next step */
    return EntryProc_FillFromLogRec( p_op, md_allow_event_updt,
                                     path_allow_event_updt );
}
#endif /* CHANGELOG support */


/**
 * check if the entry exists in the database and what info
 * must be retrieved.
 */
int EntryProc_get_info_db( struct entry_proc_op_t *p_op, lmgr_t * lmgr )
{
    int            rc;
    int            next_stage = -1; /* -1 = skip */
    const pipeline_stage_t
        *stage_info = &entry_proc_pipeline[p_op->pipeline_stage];

    /* is this a changelog record? */
    if ( p_op->extra_info.is_changelog_record )
    {
        struct changelog_rec * logrec = p_op->extra_info.log_record.p_log_rec;

        /* what do we know about this entry? */
        ATTR_MASK_INIT( &p_op->entry_attr );
        ATTR_MASK_SET( &p_op->entry_attr, fullpath );
        ATTR_MASK_SET( &p_op->entry_attr, name );
        ATTR_MASK_SET( &p_op->entry_attr, stripe_info );
        ATTR_MASK_SET( &p_op->entry_attr, md_update );
        ATTR_MASK_SET( &p_op->entry_attr, path_update );
        ATTR_MASK_SET( &p_op->entry_attr, status );

        if ( entry_proc_conf.match_classes )
        {
            /* get fileclass update info to know if we must check it */
            ATTR_MASK_SET( &p_op->entry_attr, rel_cl_update );
            ATTR_MASK_SET( &p_op->entry_attr, release_class );
            ATTR_MASK_SET( &p_op->entry_attr, arch_cl_update );
            ATTR_MASK_SET( &p_op->entry_attr, archive_class );
            p_op->entry_attr.attr_mask |= policies.purge_policies.global_attr_mask;
            p_op->entry_attr.attr_mask |= policies.migr_policies.global_attr_mask;
        }
        p_op->entry_attr.attr_mask |= entry_proc_conf.alert_attr_mask;

        rc = ListMgr_Get( lmgr, &p_op->entry_id, &p_op->entry_attr );

        if (rc == DB_SUCCESS )
        {
            p_op->db_exists = TRUE;
            p_op->entry_attr_is_set = TRUE;
        }
        else if (rc == DB_NOT_EXISTS )
        {
            p_op->db_exists = FALSE;
            ATTR_MASK_INIT( &p_op->entry_attr );
        }
        else
        {
            /* ERROR */
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                        "Error %d retrieving entry "DFID" from DB", rc,
                        PFID(&p_op->entry_id) );
            p_op->db_exists = FALSE;
            p_op->entry_attr_is_set = FALSE;
            ATTR_MASK_INIT( &p_op->entry_attr );
        }

        /* Retrieve info from the log record, and decide what info must be
         * retrieved from filesystem. */
        next_stage = EntryProc_ProcessLogRec( p_op );

        DisplayLog( LVL_DEBUG, ENTRYPROC_TAG, "RECORD: %s "DFID" %#x %s => "
                    "getstripe=%u, getattr=%u, getpath=%u, getstatus=%u",
                    changelog_type2str(logrec->cr_type), PFID(&p_op->entry_id),
                    logrec->cr_flags & CLF_FLAGMASK, logrec->cr_namelen>0?
                    logrec->cr_name:"<null>",
                    p_op->extra_info.getstripe_needed?1:0,
                    p_op->extra_info.getattr_needed?1:0,
                    p_op->extra_info.getpath_needed?1:0,
                    p_op->extra_info.getstatus_needed?1:0 );
    }
    else /* entry from FS scan */
    {
        /* scan is expected to provide full path and attributes. */
        if ( !p_op->entry_attr_is_set
             || !ATTR_MASK_TEST( &p_op->entry_attr, fullpath ) )
        {
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                        "Error: missing info from FS scan" );
            /* skip the entry */
            next_stage = -1;
            goto next_step;
        }

        /* retrieve missing attrs, if needed */
        if ( entry_proc_conf.match_classes ||
            (entry_proc_conf.alert_attr_mask & ~p_op->entry_attr.attr_mask) )
        {
            attr_set_t tmp_attr;

            ATTR_MASK_INIT( &tmp_attr );

            if ( entry_proc_conf.match_classes )
            {
                /* get fileclass update info to know if we must check it */
                ATTR_MASK_SET( &tmp_attr, rel_cl_update );
                ATTR_MASK_SET( &tmp_attr, release_class );

                tmp_attr.attr_mask |= (policies.purge_policies.global_attr_mask
                                       & ~p_op->entry_attr.attr_mask);

                ATTR_MASK_SET( &tmp_attr, arch_cl_update );
                ATTR_MASK_SET( &tmp_attr, archive_class );

                tmp_attr.attr_mask |= (policies.migr_policies.global_attr_mask
                                       & ~p_op->entry_attr.attr_mask);
            }
            tmp_attr.attr_mask |= (entry_proc_conf.alert_attr_mask
                                   & ~p_op->entry_attr.attr_mask);

            rc = ListMgr_Get( lmgr, &p_op->entry_id, &tmp_attr );

            if (rc == DB_SUCCESS )
            {
                p_op->db_exists = TRUE;
                p_op->entry_attr_is_set = TRUE;
                /* merge with main attr set */
                ListMgr_MergeAttrSets( &p_op->entry_attr, &tmp_attr );
            }
            else if (rc == DB_NOT_EXISTS )
            {
                p_op->db_exists = FALSE;
            }
            else
            {
                /* ERROR */
                DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                            "Error %d retrieving entry "DFID" from DB", rc,
                            PFID(&p_op->entry_id) );
                p_op->db_exists = FALSE;
            }
        }
        else
        {
            /* only check if the entry exists in DB? */
            p_op->db_exists = ListMgr_Exists( lmgr, &p_op->entry_id );
        }

        if ( !p_op->db_exists )
        {
            /* new entry */
            p_op->db_op_type = OP_TYPE_INSERT;
            ATTR_MASK_SET( &p_op->entry_attr, creation_time );
            ATTR( &p_op->entry_attr, creation_time ) = time( NULL );

            p_op->extra_info_is_set = TRUE;
            p_op->extra_info.getstatus_needed = TRUE;
            p_op->extra_info.getstripe_needed = TRUE;
            p_op->extra_info.getattr_needed = FALSE;
            p_op->extra_info.getpath_needed = FALSE;

            next_stage = STAGE_GET_INFO_FS;
        }
        else
        {
            p_op->db_op_type = OP_TYPE_UPDATE;

            /* FS scan is used for resynchronizing DB, so we always get status */
            p_op->extra_info_is_set = TRUE;
            p_op->extra_info.getstatus_needed = TRUE;
            p_op->extra_info.getattr_needed = FALSE;
            p_op->extra_info.getpath_needed = FALSE;

            /* Does file has stripe info ? */
            if ( ListMgr_CheckStripe( lmgr, &p_op->entry_id ) != DB_SUCCESS )
                p_op->extra_info.getstripe_needed = TRUE;

            next_stage = STAGE_GET_INFO_FS;
        }
    }

next_step:
    if ( next_stage == -1 )
        /* drop the entry */
        rc = EntryProcessor_Acknowledge( p_op, 0, TRUE );
    else
        /* go to next pipeline step */
        rc = EntryProcessor_Acknowledge( p_op, next_stage, FALSE );

    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                    "Error %d acknowledging stage %s.", rc,
                    stage_info->stage_name );
    return rc;
}



int EntryProc_get_info_fs( struct entry_proc_op_t *p_op, lmgr_t * lmgr )
{
    int            rc;

    if ( p_op->extra_info_is_set )
    {
        char           path[MAXPATHLEN];

        BuildFidPath( &p_op->entry_id, path );

#ifdef _DEBUG_ENTRYPROC
        printf( "Getattr=%d, Getpath=%d, GetStatus=%d, Getstripe=%d\n",
                p_op->extra_info.getattr_needed,
                p_op->extra_info.getpath_needed,
                p_op->extra_info.getstatus_needed, p_op->extra_info.getstripe_needed );
#endif

        if ( p_op->extra_info.getattr_needed )
        {
            struct stat    entry_md;

            rc = errno = 0;
#if defined( _MDS_STAT_SUPPORT )
           if ( global_config.direct_mds_stat )
               rc = lustre_mds_stat_by_fid( &p_op->entry_id, &entry_md );
           else
#endif
           if ( lstat( path, &entry_md ) != 0 )
              rc = errno;

            /* get entry attributes */
            if ( rc != 0 )
            {
                if ( ERR_MISSING(rc) )
                {
                    DisplayLog( LVL_FULL, ENTRYPROC_TAG, "Entry %s does not exist anymore", path );
                    /* in this case, an UNLINK event will be raised, so we ignore current record */
                    rc = EntryProcessor_Acknowledge( p_op, STAGE_CHGLOG_CLR, FALSE );
                    return rc;
                }
                else
                    DisplayLog( LVL_DEBUG, ENTRYPROC_TAG, "lstat() failed on %s: %s", path,
                                strerror( rc ) );
                /* If lstat returns an error, drop the log record */
                goto skip_record;
            }

            /* only files are considered (however, ack the record)*/
            if ( !S_ISREG( entry_md.st_mode ) )
            {
                rc = EntryProcessor_Acknowledge( p_op, STAGE_CHGLOG_CLR, FALSE );
                return rc;
            }

            /* convert them to internal structure */
#if defined( _MDS_STAT_SUPPORT )
            PosixStat2EntryAttr( &entry_md, &p_op->entry_attr, !global_config.direct_mds_stat );
#else
            PosixStat2EntryAttr( &entry_md, &p_op->entry_attr, TRUE );
#endif
            p_op->entry_attr_is_set = TRUE;

            ATTR_MASK_SET( &p_op->entry_attr, md_update );
            ATTR( &p_op->entry_attr, md_update ) = time( NULL );

        } /* getattr needed */

        if ( p_op->extra_info.getpath_needed )
        {
            /* get entry path (only for log records) */
            if ( p_op->extra_info.is_changelog_record )
            {
                rc = Lustre_GetFullPath( &p_op->entry_id,
                                         ATTR( &p_op->entry_attr, fullpath ), 1024 );

                if ( ERR_MISSING( abs( rc )) )
                {
                    DisplayLog( LVL_FULL, ENTRYPROC_TAG, "Entry "DFID" does not exist anymore", PFID(&p_op->entry_id) );
                    /* in this case, an UNLINK event will be raised, so we ignore current record */
                    goto skip_record;
                }
                else if ( rc == 0 )
                {
                    ATTR_MASK_SET( &p_op->entry_attr, fullpath );
                    ATTR_MASK_SET( &p_op->entry_attr, path_update );
                    ATTR( &p_op->entry_attr, path_update ) = time( NULL );
                }
#if 0
                else if ( !EMPTY_STRING( p_op->extra_info.log_record.p_log_rec->cr_name ) )
                {
                    /* @TODO can be useful for future path management */
                }
#endif

            }
        } /* getpath needed */

        if ( p_op->extra_info.getstripe_needed )
        {
            /* get entry stripe */
            rc = File_GetStripeByPath( path,
                                       &ATTR( &p_op->entry_attr, stripe_info ),
                                       &ATTR( &p_op->entry_attr, stripe_items ) );

            if ( ERR_MISSING( abs( rc ) )
            {
                DisplayLog( LVL_FULL, ENTRYPROC_TAG, "Entry "DFID" does not exist anymore", PFID(&p_op->entry_id) );
                /* in this case, an UNLINK event will be raised, so we ignore current record */
                goto skip_record;
            }
            else if ( rc == 0 )
            {
                ATTR_MASK_SET( &p_op->entry_attr, stripe_info );
                ATTR_MASK_SET( &p_op->entry_attr, stripe_items );
                p_op->entry_attr_is_set = TRUE;
            }
        } /* get_stripe needed */

        if ( p_op->extra_info.getstatus_needed )
        {
            /* get entry status */
            rc = LustreHSM_GetStatus( path, &ATTR( &p_op->entry_attr, status ),
                                      &ATTR( &p_op->entry_attr, no_release ),
                                      &ATTR( &p_op->entry_attr, no_archive ) );
            if ( ERR_MISSING( abs( rc )) )
            {
                DisplayLog( LVL_FULL, ENTRYPROC_TAG, "Entry "DFID" does not exist anymore", PFID(&p_op->entry_id) );
                /* in this case, an UNLINK event will be raised, so we ignore current record */
                goto skip_record;
            }
            else if ( rc == 0 )
            {
                ATTR_MASK_SET( &p_op->entry_attr, status );
                ATTR_MASK_SET( &p_op->entry_attr, no_release );
                ATTR_MASK_SET( &p_op->entry_attr, no_archive );

                /* if the entry has no flags, the entry has never been archived or restored */
                if ( ATTR( &p_op->entry_attr, status ) == STATUS_NO_FLAGS )
                {
                    ATTR_MASK_SET( &p_op->entry_attr, last_archive );
                    ATTR( &p_op->entry_attr, last_archive ) = 0;
                    ATTR_MASK_SET( &p_op->entry_attr, last_restore );
                    ATTR( &p_op->entry_attr, last_restore ) = 0;
                }

                /* if the file is released, it goes outside PolicyEngine working set */
                if ( ATTR( &p_op->entry_attr, status ) == STATUS_RELEASED )
                {
                    DisplayLog(LVL_DEBUG, ENTRYPROC_TAG,
                        "Entry "DFID" has status 'RELEASED': removing it from PolicyEngine working set (if it was in DB)",
                        PFID( &p_op->entry_id ) );
                    if ( p_op->db_exists )
                    {
                        p_op->db_op_type = OP_TYPE_REMOVE;
                    }
                    else
                    {
                        rc = EntryProcessor_Acknowledge( p_op, STAGE_CHGLOG_CLR, FALSE );
                        return rc;
                    }
                }
            }

        } /* get_status needed */
    }

    /* match fileclasses if specified in config */
    if ( entry_proc_conf.match_classes )
        check_policies( &p_op->entry_id, &p_op->entry_attr, TRUE );

    /* set other info */

    if ( p_op->extra_info.is_changelog_record )
    {
        p_op->entry_attr_is_set = TRUE;
        ATTR_MASK_SET( &p_op->entry_attr, last_op_index );
        ATTR( &p_op->entry_attr, last_op_index ) = p_op->extra_info.log_record.p_log_rec->cr_index;
    }


    rc = EntryProcessor_Acknowledge( p_op, STAGE_REPORTING, FALSE );
    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d acknowledging stage.", rc );
    return rc;

skip_record:

    /* remove the operation from pipeline */
    rc = EntryProcessor_Acknowledge( p_op, 0, TRUE );
    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d acknowledging stage.", rc );
    return rc;

}


int EntryProc_reporting( struct entry_proc_op_t *p_op, lmgr_t * lmgr )
{
    int            rc, i;
    const pipeline_stage_t *stage_info = &entry_proc_pipeline[p_op->pipeline_stage];
    int            is_alert = FALSE;
    char           stralert[2048];
    char           strvalues[2048];
    char           strid[1024];
    char         * title = NULL;

    /* generate missing fields */
    ListMgr_GenerateFields( &p_op->entry_attr,
                            entry_proc_conf.alert_attr_mask );

    /* check alert criterias (synchronously) */
    for ( i = 0; i < entry_proc_conf.alert_count; i++ )
    {
        /* check entry attr mask (else, skip it) */
        if ( !p_op->entry_attr_is_set || !p_op->entry_id_is_set
             || ( ( p_op->entry_attr.attr_mask & entry_proc_conf.alert_list[i].attr_mask ) !=
                  entry_proc_conf.alert_list[i].attr_mask ) )
            continue;

        if ( EntryMatches( &p_op->entry_id, &p_op->entry_attr,
               &entry_proc_conf.alert_list[i].boolexpr ) == POLICY_MATCH )
        {
            /* build alert string and break */
            if ( ATTR_MASK_TEST( &p_op->entry_attr, fullpath ) )
                snprintf( strid, 1024, "%s", ATTR( &p_op->entry_attr, fullpath ) );
            else
                snprintf( strid, 1024, "fid[seq, oid]=[%llu, %u]", p_op->entry_id.f_seq,
                          p_op->entry_id.f_oid );

            rc = BoolExpr2str( &entry_proc_conf.alert_list[i].boolexpr, stralert, 2048 );
            if ( rc < 0 )
                strcpy( stralert, "Error building alert string" );

            PrintAttrs( strvalues, 2048, &p_op->entry_attr,
                        entry_proc_conf.alert_list[i].attr_mask );

            if ( EMPTY_STRING(entry_proc_conf.alert_list[i].title) )
                title = NULL;
            else
                title = entry_proc_conf.alert_list[i].title;

            is_alert = TRUE;
            break;
        }
    }

    /* acknoledge now if the stage is asynchronous */
    if ( stage_info->stage_flags & STAGE_FLAG_ASYNC )
    {
        rc = EntryProcessor_Acknowledge( p_op, STAGE_DB_APPLY, FALSE );
        if ( rc )
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error acknowledging stage %s",
                        stage_info->stage_name );
    }

    if ( is_alert )
        RaiseEntryAlert(title, stralert, strid, strvalues );

    /* acknoledge now if the stage was synchronous */
    if ( !( stage_info->stage_flags & STAGE_FLAG_ASYNC ) )
    {
        rc = EntryProcessor_Acknowledge( p_op, STAGE_DB_APPLY, FALSE );
        if ( rc )
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error acknowledging stage %s",
                        stage_info->stage_name );
    }

    return 0;
}

/**
 * Perform an operation on database. 
 */
int EntryProc_db_apply( struct entry_proc_op_t *p_op, lmgr_t * lmgr )
{
    int            rc;
    const pipeline_stage_t *stage_info = &entry_proc_pipeline[p_op->pipeline_stage];

    /* if stripe has not been retrieved, don't update it in the database */
    if ( !p_op->extra_info.getstripe_needed )
        ATTR_MASK_UNSET( &p_op->entry_attr, stripe_info );

    /* insert to DB */
    switch ( p_op->db_op_type )
    {
    case OP_TYPE_INSERT:
        p_op->entry_attr.attr_mask &= ~readonly_attr_set;
        DisplayLog( LVL_FULL, ENTRYPROC_TAG, "Insert("DFID")", PFID(&p_op->entry_id) );
        rc = ListMgr_Insert( lmgr, &p_op->entry_id, &p_op->entry_attr );
        break;
    case OP_TYPE_UPDATE:
        p_op->entry_attr.attr_mask &= ~readonly_attr_set;
        DisplayLog( LVL_FULL, ENTRYPROC_TAG, "Update("DFID")", PFID(&p_op->entry_id) );
        rc = ListMgr_Update( lmgr, &p_op->entry_id, &p_op->entry_attr );
        break;
    case OP_TYPE_REMOVE:
        DisplayLog( LVL_FULL, ENTRYPROC_TAG, "Remove("DFID")", PFID(&p_op->entry_id) );
        rc = ListMgr_Remove( lmgr, &p_op->entry_id );
        break;
    case OP_TYPE_SOFT_REMOVE:
        DisplayLog( LVL_FULL, ENTRYPROC_TAG, "SoftRemove("DFID")", PFID(&p_op->entry_id) );
        rc = ListMgr_SoftRemove( lmgr, &p_op->entry_id,
                ATTR_MASK_TEST( &p_op->entry_attr, fullpath )?ATTR(&p_op->entry_attr, fullpath ):NULL,
                time(NULL) + policies.unlink_policy.deferred_remove_delay ) ;
        break;
    default:
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Unhandled DB operation type: %d", p_op->db_op_type );
        rc = -1;
    }

    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d performing database operation.", rc );

    /* Acknoledge the operation if there is a callback */
    if ( p_op->callback_func )
        rc = EntryProcessor_Acknowledge( p_op, STAGE_CHGLOG_CLR, FALSE );
    else
        rc = EntryProcessor_Acknowledge( p_op, 0, TRUE );

    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d acknoledging stage %s.", rc,
                    stage_info->stage_name );

    return rc;
}

int            EntryProc_chglog_clr( struct entry_proc_op_t * p_op, lmgr_t * lmgr )
{
    int            rc;
    const pipeline_stage_t *stage_info = &entry_proc_pipeline[p_op->pipeline_stage];
    struct changelog_rec * logrec = p_op->extra_info.log_record.p_log_rec;

    if (p_op->extra_info.is_changelog_record)
        DisplayLog( LVL_FULL, ENTRYPROC_TAG, "stage %s - record #%llu - id="DFID"\n", stage_info->stage_name,
                logrec->cr_index, PFID(&p_op->entry_id) );

    if ( p_op->callback_func )
    {
        /* if operation was commited, Perform callback to info collector */
        rc = p_op->callback_func( p_op, p_op->callback_param );

        if ( rc )
            DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d performing callback at stage %s.", rc,
                        stage_info->stage_name );
    }

    /* Acknoledge the operation and remove it from pipeline */
    rc = EntryProcessor_Acknowledge( p_op, 0, TRUE );
    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d acknoledging stage %s.", rc,
                    stage_info->stage_name );

    return rc;
}

int EntryProc_rm_old_entries( struct entry_proc_op_t *p_op, lmgr_t * lmgr )
{
    int            rc;
    const pipeline_stage_t *stage_info = &entry_proc_pipeline[p_op->pipeline_stage];
    char           timestamp[128];
    lmgr_filter_t  filter;
    filter_value_t val;

    lmgr_simple_filter_init( &filter );

    val.val_uint = ATTR( &p_op->entry_attr, md_update );
    lmgr_simple_filter_add( &filter, ATTR_INDEX_md_update, LESSTHAN_STRICT, val, 0 );

    /* force commit after this operation */
    ListMgr_ForceCommitFlag( lmgr, TRUE );

    /* remove entries listed in previous scans */
    rc = ListMgr_MassRemove( lmgr, &filter );

    lmgr_simple_filter_free( &filter );

    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG,
                    "Error: ListMgr MassRemove operation failed with code %d.", rc );
    else if ( p_op->callback_func )
    {
        /* Perform callback to info collector */
        p_op->callback_func( p_op, p_op->callback_param );
    }

    /* also update the LastScan variable */
    sprintf( timestamp, "%lu", ( unsigned long ) time( NULL ) );
    ListMgr_SetVar( lmgr, LAST_SCAN_VAR, timestamp );

    /* unset force commit flag */
    ListMgr_ForceCommitFlag( lmgr, FALSE );

    rc = EntryProcessor_Acknowledge( p_op, 0, TRUE );

    if ( rc )
        DisplayLog( LVL_CRIT, ENTRYPROC_TAG, "Error %d acknoledging stage %s.", rc,
                    stage_info->stage_name );

    return rc;

}