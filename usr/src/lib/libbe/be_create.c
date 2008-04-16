/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * System includes
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <libintl.h>
#include <libnvpair.h>
#include <libzfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mnttab.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libbe.h"
#include "libbe_priv.h"

/* Library wide variables */
libzfs_handle_t *g_zfs = NULL;

/* Private function prototypes */
static int be_clone_fs_callback(zfs_handle_t *, void *);
static int be_destroy_callback(zfs_handle_t *, void *);
static int be_send_fs_callback(zfs_handle_t *, void *);
static int be_get_snap(char *, char **);
static boolean_t be_create_container_ds(char *);


/* ********************************************************************	*/
/*			Public Functions				*/
/* ********************************************************************	*/

/*
 * Function:	be_init
 * Description:	Creates the initial datasets for a BE and leaves them
 *		unpopulated.  The resultant BE can be mounted but can't
 *		yet be activated or booted.
 * Parameters:
 *		be_attrs - pointer to nvlist_t of attributes being passed in.
 *			The following attributes are used by this function:
 *
 *			BE_ATTR_NEW_BE_NAME		*required
 *			BE_ATTR_NEW_BE_POOL		*required
 *			BE_ATTR_ZFS_PROPERTIES		*optional
 *			BE_ATTR_FS_NAMES		*optional
 *			BE_ATTR_FS_NUM			*optional
 *			BE_ATTR_SHARED_FS_NAMES		*optional
 *			BE_ATTR_SHARED_FS_NUM		*optional
 * Return:
 *		0 - Success
 *		non-zero - Failure
 * Scope:
 *		Public
 */
int
be_init(nvlist_t *be_attrs)
{
	be_transaction_data_t	bt = { 0 };
	zpool_handle_t	*zlp;
	zfs_handle_t	*zhp;
	nvlist_t	*zfs_props = NULL;
	char		nbe_root_ds[MAXPATHLEN];
	char		child_fs[MAXPATHLEN];
	char		**fs_names = NULL;
	char		**shared_fs_names = NULL;
	int		fs_num = 0;
	int		shared_fs_num = 0;
	int		nelem;
	int		i;
	int		zret;
	int		ret = 0;

	/* Initialize libzfs handle */
	if (!be_zfs_init())
		return (1);

	/* Get new BE name */
	if (nvlist_lookup_string(be_attrs, BE_ATTR_NEW_BE_NAME, &bt.nbe_name)
	    != 0) {
		be_print_err(gettext("be_init: failed to lookup "
		    "BE_ATTR_NEW_BE_NAME attribute\n"));
		return (1);
	}

	/* Validate new BE name */
	if (!be_valid_be_name(bt.nbe_name)) {
		be_print_err(gettext("be_init: invalid BE name %s\n"),
		    bt.nbe_name);
		return (1);
	}

	/* Get zpool name */
	if (nvlist_lookup_string(be_attrs, BE_ATTR_NEW_BE_POOL, &bt.nbe_zpool)
	    != 0) {
		be_print_err(gettext("be_init: failed to lookup "
		    "BE_ATTR_NEW_BE_POOL attribute\n"));
		return (1);
	}

	/* Get file system attributes */
	nelem = 0;
	if (nvlist_lookup_pairs(be_attrs, 0,
	    BE_ATTR_FS_NUM, DATA_TYPE_UINT16, &fs_num,
	    BE_ATTR_FS_NAMES, DATA_TYPE_STRING_ARRAY, &fs_names, &nelem,
	    NULL) != 0) {
		be_print_err(gettext("be_init: failed to lookup fs "
		    "attributes\n"));
		return (1);
	}
	if (nelem != fs_num) {
		be_print_err(gettext("be_init: size of FS_NAMES array does not "
		    "match FS_NUM\n"));
		return (1);
	}

	/* Get shared file system attributes */
	nelem = 0;
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_SHARED_FS_NUM, DATA_TYPE_UINT16, &shared_fs_num,
	    BE_ATTR_SHARED_FS_NAMES, DATA_TYPE_STRING_ARRAY, &shared_fs_names,
	    &nelem, NULL) != 0) {
		be_print_err(gettext("be_init: failed to lookup "
		    "shared fs attributes\n"));
		return (1);
	}
	if (nelem != shared_fs_num) {
		be_print_err(gettext("be_init: size of SHARED_FS_NAMES "
		    "array does not match SHARED_FS_NUM\n"));
		return (1);
	}

	/* Verify that nbe_zpool exists */
	if ((zlp = zpool_open(g_zfs, bt.nbe_zpool)) == NULL) {
		be_print_err(gettext("be_init: failed to "
		    "find existing zpool (%s)\n"), bt.nbe_zpool);
		return (1);
	}
	zpool_close(zlp);

	/*
	 * Verify BE container dataset in nbe_zpool exists.
	 * If not, create it.
	 */
	if (!be_create_container_ds(bt.nbe_zpool))
		return (1);

	/*
	 * Verify that nbe_name doesn't already exist in some pool.
	 */
	if ((zret = zpool_iter(g_zfs, be_exists_callback, bt.nbe_name)) > 0) {
		be_print_err(gettext("be_init: BE (%s) already exists\n"),
		    bt.nbe_name);
		return (1);
	} else if (zret < 0) {
		be_print_err(gettext("be_init: zpool_iter failed: %s\n"),
		    libzfs_error_description(g_zfs));
		return (1);
	}

	/* Generate string for BE's root dataset */
	be_make_root_ds(bt.nbe_zpool, bt.nbe_name, nbe_root_ds,
	    sizeof (nbe_root_ds));

	/*
	 * Create property list for new BE root dataset.  If some
	 * zfs properties were already provided by the caller, dup
	 * that list.  Otherwise initialize a new property list.
	 */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_ZFS_PROPERTIES, DATA_TYPE_NVLIST, &zfs_props, NULL)
	    != 0) {
		be_print_err(gettext("be_init: failed to lookup "
		    "BE_ATTR_ZFS_PROPERTIES attribute\n"));
		return (1);
	}
	if (zfs_props != NULL) {
		/* Make sure its a unique nvlist */
		if(!(zfs_props->nvl_nvflag & NV_UNIQUE_NAME) &&
		   !(zfs_props->nvl_nvflag & NV_UNIQUE_NAME_TYPE)) {
			be_print_err(gettext("be_init: ZFS property list "
			    "not unique\n"));
			return (1);
		}

		/* Dup the list */
		if (nvlist_dup(zfs_props, &bt.nbe_zfs_props, 0) != 0) {
			be_print_err(gettext("be_init: failed to dup ZFS "
			    "property list\n"));
			return (1);
		}
	} else {
		/* Initialize new nvlist */
		if (nvlist_alloc(&bt.nbe_zfs_props, NV_UNIQUE_NAME, 0) != 0) {
			be_print_err(gettext("be_init: internal "
			    "error: out of memory\n"));
			return (1);
		}
	}

	/*
	 * TODO - change this to "/" when zfs boot integrates
	 */
	if (nvlist_add_string(bt.nbe_zfs_props,
	    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT), ZFS_MOUNTPOINT_LEGACY)
	    != 0) {
		be_print_err(gettext("be_init: internal error "
		    "out of memory\n"));
		ret = 1;
		goto done;
	}

	/* Set the 'canmount' property */
	if (nvlist_add_string(bt.nbe_zfs_props,
	    zfs_prop_to_name(ZFS_PROP_CANMOUNT), "noauto") != 0) {
		be_print_err(gettext("be_init: internal error "
		    "out of memory\n"));
		ret = 1;
		goto done;
	}

	/* Create BE root dataset for the new BE */
	if (zfs_create(g_zfs, nbe_root_ds, ZFS_TYPE_FILESYSTEM,
	    bt.nbe_zfs_props) != 0) {
		be_print_err(gettext("be_init: failed to "
		    "create BE root dataset (%s): %s\n"), nbe_root_ds,
		     libzfs_error_description(g_zfs));
		ret = 1;
		goto done;
	}

	/* Create the new BE's non-shared file systems */
	for (i = 0; i < fs_num && fs_names[i]; i++) {
		/*
		 * If fs == "/", skip it;
		 * we already created the root dataset
		 */
		if (strcmp(fs_names[i], "/") == 0)
			continue;

		/*
		 * TODO - Make the mountpoints inherited after
		 * zfs boot integrates.
		 */
		if (nvlist_add_string(bt.nbe_zfs_props,
		    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT), fs_names[i]) != 0) {
			be_print_err(gettext("be_init: "
			    "internal error: out of memory\n"));
			ret = 1;
			goto done;
		}

		/* Generate string for file system */
		(void) snprintf(child_fs, sizeof (child_fs), "%s%s",
		    nbe_root_ds, fs_names[i]);

		/* Create file system */
		if (zfs_create(g_zfs, child_fs, ZFS_TYPE_FILESYSTEM,
		    bt.nbe_zfs_props) != 0) {
			be_print_err(gettext("be_init: failed to create "
			    "BE's child dataset (%s): %s\n"), child_fs,
			    libzfs_error_description(g_zfs));
			ret = 1;
			goto done;
		}
	}

	/* Create the new BE's shared file systems */
	if (shared_fs_num > 0) {
		nvlist_t	*props = NULL;

		if (nvlist_alloc(&props, NV_UNIQUE_NAME, 0) != 0) {
			be_print_err(gettext("be_init: nvlist_alloc failed\n"));
			ret = 1;
			goto done;
		}

		for (i = 0; i < shared_fs_num; i++) {
			/* Generate string for shared file system */
			(void) snprintf(child_fs, sizeof (child_fs), "%s%s",
			    bt.nbe_zpool, shared_fs_names[i]);

			if (nvlist_add_string(props,
			    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT),
			    shared_fs_names[i]) != 0) {
				be_print_err(gettext("be_init: "
				    "internal error: out of memory\n"));
				nvlist_free(props);
				ret = 1;
				goto done;
			}

			/* Create file system if it doesn't already exist */
			if (zfs_dataset_exists(g_zfs, child_fs,
			    ZFS_TYPE_FILESYSTEM)) {
				continue;
			}
			if (zfs_create(g_zfs, child_fs, ZFS_TYPE_FILESYSTEM,
			    props) != 0) {
				be_print_err(gettext("be_init: failed to "
				    "create BE's shared dataset (%s): %s\n"),
				    child_fs, libzfs_error_description(g_zfs));
				nvlist_free(props);
				ret = 1;
				goto done;
			}
		}

		nvlist_free(props);
	}

done:
	if (bt.nbe_zfs_props != NULL)
		nvlist_free(bt.nbe_zfs_props);

	be_zfs_fini();

	return (ret);
}

/*
 * Function:	be_destroy
 * Description:	Destroy a BE and all of its children datasets and snapshots.
 * Parameters:
 *		be_attrs - pointer to nvlist_t of attributes being passed in.
 *			The following attributes are used by this function:
 *
 *			BE_ATTR_ORIG_BE_NAME		*required
 * Return:
 *		0 - Success
 *		non-zero - Failure
 * Scope:
 *		Public
 *
 * NOTES - Requires that the BE being deleted has no dependent BEs.  If it
 *	   does, the destroy will fail.
 */
int
be_destroy(nvlist_t *be_attrs)
{
	be_transaction_data_t	bt = { 0 };
	zfs_handle_t	*zhp;
	char		obe_root_ds[MAXPATHLEN];
	char		origin[MAXPATHLEN];
	char		parent[MAXPATHLEN];
	char		*mp = NULL;
	char		*snap = NULL;
	boolean_t	has_origin = B_FALSE;
	char		numclonestr[BUFSIZ];
	uint64_t	numclone;
	int		zret;
	int		ret = 0;

	/* Initialize libzfs handle */
	if (!be_zfs_init())
		return (1);

	/* Get name of BE to delete */
	if (nvlist_lookup_string(be_attrs, BE_ATTR_ORIG_BE_NAME, &bt.obe_name)
	    != 0) {
		be_print_err(gettext("be_destroy: failed to lookup "
		    "BE_ATTR_ORIG_BE_NAME attribute\n"));
		return (1);
	}

	/* Validate  BE name */
	if (!be_valid_be_name(bt.obe_name)) {
		be_print_err(gettext("be_destroy: invalid BE name %s\n"),
		    bt.obe_name);
		return (1);
	}

	/* Find which zpool obe_name lives in */
	if ((zret = zpool_iter(g_zfs, be_find_zpool_callback, &bt)) == 0) {
		be_print_err(gettext("be_destroy: failed to find zpool "
		    "for BE (%s)\n"), bt.obe_name);
		return (1);
	} else if (zret < 0) {
		be_print_err(gettext("be_destroy: zpool_iter failed: %s\n"),
		    libzfs_error_description(g_zfs));
		return (1);
	}

	/* Generate string for obe_name's root dataset */
	be_make_root_ds(bt.obe_zpool, bt.obe_name, obe_root_ds,
	    sizeof (obe_root_ds));
	bt.obe_root_ds = obe_root_ds;

	/* Get handle to BE's root dataset */
	if ((zhp = zfs_open(g_zfs, bt.obe_root_ds, ZFS_TYPE_FILESYSTEM)) ==
	    NULL) {
		be_print_err(gettext("be_destroy: failed to "
		    "open BE root dataset (%s)\n"), bt.obe_root_ds);
		return (1);
	}

	/* Is BE mounted */
	if (zfs_is_mounted(zhp, &mp)) {
		be_print_err(gettext("be_destroy: %s is currently mounted "
		    "at %s, cannot destroy\n"), bt.obe_name,
		    mp != NULL ? mp : "");
		if (mp != NULL)
			free(mp);
		return (1);
	}

	/*
	 * Get the parent of this BE's root dataset.  This will be used
	 * later to destroy the snapshots originally used to create this BE.
	 */
	if (zfs_prop_get(zhp, ZFS_PROP_ORIGIN, origin, sizeof (origin), NULL,
	    NULL, 0, B_FALSE) == 0) {
		(void) strlcpy(parent, origin, sizeof (parent));
		if (be_get_snap(parent, &snap) != 0) {
			be_print_err(gettext("be_destroy: failed to "
			    "get snapshot name from origin\n"));
			return (1);
		}
		has_origin = B_TRUE;
	}

	/* Destroy the BE's root and its hierarchical children */
	if (be_destroy_callback(zhp, NULL) != 0) {
		zfs_close(zhp);
		be_print_err(gettext("be_destroy: failed to "
		    "destroy BE root dataset (%s)\n"), bt.obe_root_ds);
		return (1);
	}

	/* If BE has an origin */
	if (has_origin) {

		/*
		 * If origin snapshot name is equal to the name of the BE
		 * we just deleted, and the origin doesn't have any other
		 * dependents, delete the origin.
		 */
		if (strcmp(snap, bt.obe_name) != 0)
			goto done;

		if ((zhp = zfs_open(g_zfs, origin, ZFS_TYPE_SNAPSHOT)) ==
		    NULL) {
			be_print_err(gettext("be_destroy: failed to "
			    "open BE's origin (%s)\n"), origin);
			ret = 1;
			goto done;
		}

		/* Get the number of clones this origin snapshot has */
		if (zfs_prop_get(zhp, ZFS_PROP_NUMCLONES, numclonestr,
		    sizeof(numclonestr), NULL, NULL, 0, B_TRUE) != 0) {
			be_print_err(gettext("be_destroy: failed to "
			    "get number of clones for %s\n"), origin);
			ret = 1;
			goto done;
		}

		zfs_close(zhp);

		if (sscanf(numclonestr, "%llu", &numclone) != 1) {
			be_print_err(gettext("be_destroy: invalid numclone "
			    "format %s\n"), numclonestr);
			ret = 1;
			goto done;
		}

		/* If origin has dependents, don't delete it. */
		if (numclone != 0)
			goto done;

		/* Get handle to BE's parent's root dataset */
		if ((zhp = zfs_open(g_zfs, parent, ZFS_TYPE_FILESYSTEM)) ==
		    NULL) {
			be_print_err(gettext("be_destroy: failed to "
			    "open BE's parent root dataset (%s)\n"), parent);
			ret = 1;
			goto done;
		}

		/* Destroy the snapshot origin used to create this BE. */
		if (zfs_destroy_snaps(zhp, snap) != 0) {
			be_print_err(gettext("be_destroy: failed to "
			    "destroy original snapshots used to create BE\n"));
			ret = 1;
			goto done;
		}

		zfs_close(zhp);
	}

 done:
	/* Remove BE's entry from the GRUB menu */
	if (be_remove_grub(bt.obe_name, bt.obe_zpool, NULL) != 0) {
		be_print_err(gettext("be_destroy: failed to "
		    "remove BE %s from the GRUB menu\n"), bt.obe_name);
		return (1);
	}

	be_zfs_fini();

	return (ret);
}

/*
 * Function:	be_copy
 * Description:	This function makes a copy of an existing BE.  If the original
 *		BE and the new BE are in the same pool, it uses zfs cloning to
 *		create the new BE, otherwise it does a physical copy.
 *		If the original BE name isn't provided, it uses the currently
 *		booted BE.  If the new BE name isn't provided, it creates an
 *		auto named BE and returns that name to the caller.
 * Parameters:
 *		be_attrs - pointer to nvlist_t of attributes being passed in.
 *			The following attributes are used by this function:
 *
 *			BE_ATTR_ORIG_BE_NAME		*optional
 *			BE_ATTR_SNAP_NAME		*optional
 *			BE_ATTR_NEW_BE_NAME		*optional
 *			BE_ATTR_NEW_BE_POOL		*optional
 *			BE_ATTR_NEW_BE_DESC		*optional
 *			BE_ATTR_ZFS_PROPERTIES		*optional
 *			BE_ATTR_POLICY			*optional
 *
 *			If the BE_ATTR_NEW_BE_NAME was not passed in, upon
 *			successful BE creation, the following attribute values
 *			will be returned to the caller by setting them in the
 *			be_attrs parameter passed in:
 *
 *			BE_ATTR_SNAP_NAME
 *			BE_ATTR_NEW_BE_NAME
 * Return:
 *		0 - Success
 *		non-zero - Failure
 * Scope:
 *		Public
 */
int
be_copy(nvlist_t *be_attrs)
{
	be_transaction_data_t	bt = { 0 };
	zfs_handle_t	*zhp = NULL;
	nvlist_t	*zfs_props = NULL;
	char		obe_zpool[MAXPATHLEN];
	char		obe_root_ds[MAXPATHLEN];
	char		nbe_root_ds[MAXPATHLEN];
	char		ss[MAXPATHLEN];
	boolean_t	autoname = B_FALSE;
	int		i;
	int		zret;
	int		ret = 0;

	/* Initialize libzfs handle */
	if (!be_zfs_init())
		return (1);

	/* Get original BE name */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_ORIG_BE_NAME, DATA_TYPE_STRING, &bt.obe_name, NULL) != 0) {
		be_print_err(gettext("be_copy: failed to lookup "
		    "BE_ATTR_ORIG_BE_NAME attribute\n"));
		return (1);
	}

	/* If original BE name not provided, use current BE */
	if (bt.obe_name == NULL) {
		if ((zret = zpool_iter(g_zfs,
		    be_zpool_find_current_be_callback, &bt)) == 0) {
			be_print_err(gettext("be_copy: failed to "
			    "find current BE name\n"));
			return (1);
		} else if (zret < 0) {
			be_print_err(gettext("be_copy: "
			    "zpool_iter failed: %s\n"),
			    libzfs_error_description(g_zfs));
			return (1);
		}
	} else {
		/* Validate original BE name */
		if (!be_valid_be_name(bt.obe_name)) {
			be_print_err(gettext("be_copy: "
			    "invalid BE name %s\n"), bt.obe_name);
			return (1);
		}
	}

	/* Find which zpool obe_name lives in */
	if ((zret = zpool_iter(g_zfs, be_find_zpool_callback, &bt)) == 0) {
		be_print_err(gettext("be_copy: failed to "
		    "find zpool for BE (%s)\n"), bt.obe_name);
		return (1);
	} else if (zret < 0) {
		be_print_err(gettext("be_copy: "
		    "zpool_iter failed: %s\n"),
		    libzfs_error_description(g_zfs));
		return (1);
	}

	/* Get snapshot name of original BE if one was provided */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_SNAP_NAME, DATA_TYPE_STRING, &bt.obe_snap_name, NULL)
	    != 0) {
		be_print_err(gettext("be_copy: failed to lookup "
		    "BE_ATTR_SNAP_NAME attribute\n"));
		return (1);
	}

	/* Get new BE name */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_NEW_BE_NAME, DATA_TYPE_STRING, &bt.nbe_name, NULL)
	    != 0) {
		be_print_err(gettext("be_copy: failed to lookup "
		    "BE_ATTR_NEW_BE_NAME attribute\n"));
		return (1);
	}

	/* Get zpool name to create new BE in */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_NEW_BE_POOL, DATA_TYPE_STRING, &bt.nbe_zpool, NULL) != 0) {
		be_print_err(gettext("be_copy: failed to lookup "
		    "BE_ATTR_NEW_BE_POOL attribute\n"));
		return (1);
	}

	/* Get new BE's description if one was provided */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_NEW_BE_DESC, DATA_TYPE_STRING, &bt.nbe_desc, NULL) != 0 ) {
		be_print_err(gettext("be_copy: failed to lookup "
		    "BE_ATTR_NEW_BE_DESC attribute\n"));
		return (1);
	}

	/* Get BE policy to create this snapshot under */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_POLICY, DATA_TYPE_STRING, &bt.policy, NULL) != 0) {
		be_print_err(gettext("be_copy: failed to lookup "
		    "BE_ATTR_POLICY attribute\n"));
		return (1);
	}
	if (bt.policy == NULL) {
		/* If no policy type provided, use default type */
		bt.policy = be_default_policy();
	}

	/*
	 * Create property list for new BE root dataset.  If some
	 * zfs properties were already provided by the caller, dup
	 * that list.  Otherwise initialize a new property list.
	 */
	if (nvlist_lookup_pairs(be_attrs, NV_FLAG_NOENTOK,
	    BE_ATTR_ZFS_PROPERTIES, DATA_TYPE_NVLIST, &zfs_props, NULL)
	    != 0) {
		be_print_err(gettext("be_copy: failed to lookup "
		    "BE_ATTR_ZFS_PROPERTIES attribute\n"));
		return (1);
	}
	if (zfs_props != NULL) {
		/* Make sure its a unique nvlist */
		if (!(zfs_props->nvl_nvflag & NV_UNIQUE_NAME) &&
		    !(zfs_props->nvl_nvflag & NV_UNIQUE_NAME_TYPE)) {
			be_print_err(gettext("be_copy: ZFS property list "
			    "not unique\n"));
			return (1);
		}

		/* Dup the list */
		if (nvlist_dup(zfs_props, &bt.nbe_zfs_props, 0) != 0) {
			be_print_err(gettext("be_copy: "
			    "failed to dup ZFS property list\n"));
			return (1);
		}
	} else {
		/* Initialize new nvlist */
		if (nvlist_alloc(&bt.nbe_zfs_props, NV_UNIQUE_NAME, 0) != 0) {
			be_print_err(gettext("be_copy: internal "
			    "error: out of memory\n"));
			return (1);
		}
	}

	/*
	 * If new BE name provided, validate the BE name and then verify
	 * that new BE name doesn't already exist in some pool.
	 */
	if (bt.nbe_name) {
		/* Validate original BE name */
		if (!be_valid_be_name(bt.nbe_name)) {
			be_print_err(gettext("be_copy: "
			    "invalid BE name %s\n"), bt.nbe_name);
			ret = 1;
			goto done;
		}

		/* Verify it doesn't already exist */
		if ((zret = zpool_iter(g_zfs, be_exists_callback, bt.nbe_name))
		    > 0) {
			be_print_err(gettext("be_copy: BE (%s) already "
			    "exists\n"), bt.nbe_name);
			ret = 1;
			goto done;
		} else if (zret < 0) {
			be_print_err(gettext("be_copy: zpool_iter failed: "
			    "%s\n"), libzfs_error_description(g_zfs));
			ret = 1;
			goto done;
		}
	} else {
		/*
		 * If an auto named BE is desired, it must be in the same
		 * pool is the original BE, and cannot be based on some
		 * existing snapshot.
		 */
		if (bt.nbe_zpool != NULL) {
			be_print_err(gettext("be_copy: cannot specify pool "
			    "name when creating an auto named BE\n"));
			ret = 1;
			goto done;
		}
		if (bt.obe_snap_name != NULL) {
			be_print_err(gettext("be_copy: cannot specify "
			    "a snapshot name when creating an "
			    "auto named BE\n"));
			ret = 1;
			goto done;
		}

		/*
		 * Generate auto named BE
		 */
		if ((bt.nbe_name = be_auto_be_name(bt.obe_name))
		    == NULL) {
			be_print_err(gettext("be_copy: "
			    "failed to generate auto BE name\n"));
			ret = 1;
			goto done;
		}

		autoname = B_TRUE;
	}

	/*
	 * If zpool name to create new BE in is not provided,
	 * create new BE in original BE's pool.
	 */
	if (bt.nbe_zpool == NULL) {
		bt.nbe_zpool = bt.obe_zpool;
	}

	/* Get root dataset names for obe_name and nbe_name */
	be_make_root_ds(bt.obe_zpool, bt.obe_name, obe_root_ds,
	    sizeof (obe_root_ds));
	be_make_root_ds(bt.nbe_zpool, bt.nbe_name, nbe_root_ds,
	    sizeof (nbe_root_ds));

	bt.obe_root_ds = obe_root_ds;
	bt.nbe_root_ds = nbe_root_ds;

	/*
	 * If an existing snapshot name has been provided to create from,
	 * verify that it exists for the original BE's root dataset.
	 */
	if (bt.obe_snap_name != NULL) {

		/* Generate dataset name for snapshot to use. */
		(void) snprintf(ss, sizeof (ss), "%s@%s", bt.obe_root_ds,
		    bt.obe_snap_name);

		/* Verify snapshot exists */
		if (!zfs_dataset_exists(g_zfs, ss, ZFS_TYPE_SNAPSHOT)) {
			be_print_err(gettext("be_copy: "
			    "snapshot does not exist (%s): %s\n"), ss,
			    libzfs_error_description(g_zfs));
			ret = 1;
			goto done;
		}
	} else {
		/*
		 * Else snapshot name was not provided, if we're creating
		 * an auto named BE, generate an auto named snapshot to
		 * use as its origin, otherwise just use the new BE name
		 * as the snapshot name.
		 */
		if (autoname) {
			if (_be_create_snapshot(bt.obe_name, &bt.obe_snap_name,
			    bt.policy) != 0) {
				be_print_err(gettext("be_copy: "
				    "failed to create auto named snapshot\n"));
				ret = 1;
				goto done;
			}

			if (nvlist_add_string(be_attrs, BE_ATTR_SNAP_NAME,
			    bt.obe_snap_name) != 0) {
				be_print_err(gettext("be_copy: "
				    "failed to add snap name to be_attrs\n"));
				ret = 1;
				goto done;
			}
		} else {
			bt.obe_snap_name = bt.nbe_name;

			/*
			 * Generate the string for the snapshot to take.
			 */
			(void) snprintf(ss, sizeof (ss), "%s@%s",
			    bt.obe_root_ds, bt.obe_snap_name);

			/*
			 * Take a recursive snapshot of the original BE.
			 */
			if (zfs_snapshot(g_zfs, ss, B_TRUE)) {
				be_print_err(gettext("be_copy: "
				    "failed to snapshot BE (%s): %s\n"),
				    ss, libzfs_error_description(g_zfs));
				ret = 1;
				goto done;
			}
		}
	}

	if (strcmp(bt.obe_zpool, bt.nbe_zpool) == 0) {

		/* Do clone */

		/* get handle to obe_name's root dataset. */
		if ((zhp = zfs_open(g_zfs, bt.obe_root_ds,
		    ZFS_TYPE_FILESYSTEM)) == NULL) {
			be_print_err(gettext("be_copy: failed to "
			    "open BE root dataset (%s)\n"), bt.obe_root_ds);
			ret = 1;
			goto done;
		}

		/*
		 * Iterate through original BE's datasets and clone 
		 * them to create new BE.
		 */
		if ((zret = be_clone_fs_callback(zhp, &bt)) != 0) {

			/* Creating clone BE failed */
			if (!autoname || zret != BE_ERR_EXISTS) {
				be_print_err(gettext("be_copy: "
				    "failed to clone new BE (%s) from "
				    "orig BE (%s)\n"),
				    bt.nbe_name, bt.obe_name);
				ret = 1;
				goto done;
			} else {
				for (i = 1; i < BE_AUTO_NAME_MAX_TRY; i++) {

					/* Sleep 1 before retrying */
					sleep (1);

					/* Generate new auto BE name */
					free(bt.nbe_name);
					if ((bt.nbe_name =
					    be_auto_be_name(bt.obe_name))
					    == NULL) {
						be_print_err(gettext("be_copy: "
						    "failed to generate auto "
						    "BE name\n"));
						ret = 1;
						goto done;
					}

					/*
					 * Regenerate string for new BE's
					 * root dataset name
					 */
					be_make_root_ds(bt.nbe_zpool,
					     bt.nbe_name, nbe_root_ds,
					     sizeof (nbe_root_ds));
					bt.nbe_root_ds = nbe_root_ds;

					/* Try to clone BE again. */
					zret = be_clone_fs_callback(zhp, &bt);
					if (zret == 0) {
						break;
					} else if (zret != BE_ERR_EXISTS) {
						be_print_err(gettext("be_copy: "
						    "failed to clone new BE "
						    "(%s) from orig BE (%s)\n"),
						    bt.nbe_name, bt.obe_name);
						ret = 1;
						goto done;
					}
				}

				/*
				 * If we've exhausted the maximum number of
				 * tries, free the auto BE name and return
				 * error.
				 */
				if (i == BE_AUTO_NAME_MAX_TRY) {
					be_print_err(gettext("be_copy: failed "
					    "to create unique auto BE name\n"));
					free(bt.nbe_name);
					bt.nbe_name = NULL;
					ret = 1;
					goto done;
				}
			}
		}

		zfs_close(zhp);

		/*
		 * Process zones outside of the private BE namespace.
		 * - Not supported yet.
		 */

	} else {

		/* Do copy (i.e. send BE datasets via zfs_send/recv) */

		/*
		 * Verify BE container dataset in nbe_zpool exists.
		 * If not, create it.
		 */
		if (!be_create_container_ds(bt.nbe_zpool)) {
			ret = 1;
			goto done;
		}

		/* Get handle to original BE's root dataset. */
		if ((zhp = zfs_open(g_zfs, bt.obe_root_ds,
		    ZFS_TYPE_FILESYSTEM)) == NULL) {
			be_print_err(gettext("be_copy: failed to "
			    "open BE root dataset (%s)\n"), bt.obe_root_ds);
			ret = 1;
			goto done;
		}

		/*
		 * Iterate through original BE's datasets and send
		 * them to the other pool.
		 */
		if ((zret = be_send_fs_callback(zhp, &bt)) != 0) {
			be_print_err(gettext("be_copy: failed to "
			    "send BE (%s) to pool (%s)\n"), bt.obe_name,
			    bt.nbe_zpool);
			ret = 1;
			goto done;
		}

		zfs_close(zhp);

		/*
		 * Process zones outside of the private BE namespace.
		 * - Not supported yet.
		 */
	}

	/*
	 * TODO - Until we have ZFS boot, we need to modify the new BE's
	 * vfstab because we're still legacy mounting root.
	 */
	if (be_update_vfstab(bt.nbe_name, bt.nbe_root_ds, NULL) != 0) {
		be_print_err(gettext("be_copy: failed to "
		    "update new BE's vfstab (%s)\n"), bt.nbe_name);
		ret = 1;
		goto done;
	}

	/*
	 * Add GRUB entry for newly created clone
	 */
	if (be_append_grub(bt.nbe_name, bt.nbe_zpool, NULL, bt.nbe_desc) != 0) {
		be_print_err(gettext("be_copy: failed to "
		    "add BE (%s) to GRUB menu\n"), bt.nbe_name);
		ret = 1;
		goto done;
	}

	/*
	 * If we succeeded in creating an auto named BE, set its policy
	 * type and return the auto generated name to the caller by storing
	 * it in the nvlist passed in by the caller.
	 */
	if (autoname) {
		/* Get handle to new BE's root dataset. */
		if ((zhp = zfs_open(g_zfs, bt.nbe_root_ds,
		    ZFS_TYPE_FILESYSTEM)) == NULL) {
			be_print_err(gettext("be_copy: failed to "
			    "open BE root dataset (%s)\n"), bt.nbe_root_ds);
			ret = 1;
			goto done;
		}

		/*
		 * Set the policy type property into the new BE's root dataset
		 */
		if (zfs_prop_set(zhp, BE_POLICY_PROPERTY, bt.policy) != 0) {
			be_print_err(gettext("be_copy: failed to "
			    "set BE policy for %s\n"), bt.nbe_name);
			ret = 1;
			goto done;
		}

		zfs_close(zhp);

		/*
		 * Return the auto generated name to the caller
		 */
		if (bt.nbe_name) {
			if (nvlist_add_string(be_attrs, BE_ATTR_NEW_BE_NAME,
			    bt.nbe_name) != 0) {
				be_print_err(gettext("be_copy: failed to "
				    "add snap name to be_attrs\n"));
			}
		}
	}

done:
	if (bt.nbe_zfs_props != NULL)
		nvlist_free(bt.nbe_zfs_props);

	be_zfs_fini(); 

	return (ret);
}

/* ********************************************************************	*/
/*			Semi-Private Functions				*/
/* ******************************************************************** */

/*
 * Function:	be_find_zpool_callback
 * Description:	Callback function used to find the pool that a BE lives in.
 * Parameters:
 *		zlp - zpool_handle_t pointer for the current pool being
 *			looked at.
 *		data - be_transaction_data_t pointer providing information
 *			about the BE that's being searched for.
 *			This function uses the obe_name member of this
 *			parameter to use as the BE name to search for.
 *			Upon successfully locating the BE, it populates
 *			obe_zpool with the pool name that the BE is found in.
 * Returns:
 *		1 - BE exists in this pool.
 *		0 - BE does not exist in this pool.
 * Scope:
 *		Semi-private (library wide use only)
 */
int
be_find_zpool_callback(zpool_handle_t *zlp, void *data)
{
	be_transaction_data_t	*bt = data;
	const char		*zpool =  zpool_get_name(zlp);
	char			be_root_ds[MAXPATHLEN];

	/*
	 * Generate string for the BE's root dataset
	 */
	be_make_root_ds(zpool, bt->obe_name, be_root_ds, sizeof (be_root_ds));

	/*
	 * Check if dataset exists
	 */
	if (zfs_dataset_exists(g_zfs, be_root_ds, ZFS_TYPE_FILESYSTEM)) {
		/* BE's root dataset exists in zpool */
		bt->obe_zpool = strdup(zpool);
		return (1);
	}

	return (0);
}

/*
 * Function:	be_exists_callback
 * Description:	Callback function used to find out if a BE exists.
 * Parameters:
 *		zlp - zpool_handle_t pointer to the current pool being
 *			looked at.
 *		data - BE name to look for.
 * Return:
 *		1 - BE exists in this pool.
 *		0 - BE does not exist in this pool.
 * Scope:
 *		Semi-private (library wide use only)
 */
int
be_exists_callback(zpool_handle_t *zlp, void *data)
{
	const char	*zpool = zpool_get_name(zlp);
	char		*be_name = data;
	char		be_root_ds[MAXPATHLEN];

	/*
	 * Generate string for the BE's root dataset
	 */
	be_make_root_ds(zpool, be_name, be_root_ds, sizeof (be_root_ds));

	/*
	 * Check if dataset exists
	 */
	if (zfs_dataset_exists(g_zfs, be_root_ds, ZFS_TYPE_FILESYSTEM)) {
		/* BE's root dataset exists in zpool */
		return (1);
	}

	return (0);
}

/*
 * Function:	be_zpool_find_current_be_callback
 * Description: Callback function used to iterate through all existing pools
 *		to find the BE that is the currently booted BE.
 * Parameters:
 *		zlp - zpool_handle_t pointer to the current pool being
 *			looked at.
 *		data - be_transaction_data_t pointer.
 *			Upon successfully finding the current BE, the
 *			obe_zpool member of this parameter is set to the
 *			pool it is found in.
 * Return:
 *		1 - Found current BE in this pool.
 *		0 - Did not find current BE in this pool.
 * Scope:
 *		Semi-private (library wide use only)
 */
int
be_zpool_find_current_be_callback(zpool_handle_t *zlp, void *data)
{
	be_transaction_data_t	*bt = data;
	zfs_handle_t		*zhp = NULL;
	const char		*zpool =  zpool_get_name(zlp);
	char			be_container_ds[MAXPATHLEN];
	int			ret = 0;

	/*
	 * Generate string for BE container dataset
	 */
	be_make_container_ds(zpool, be_container_ds, sizeof (be_container_ds));

	/*
	 * Check if a BE container dataset exists in this pool.
	 */
	if (!zfs_dataset_exists(g_zfs, be_container_ds, ZFS_TYPE_FILESYSTEM))
		return (0);

	/*
	 * Get handle to this zpool's BE container dataset.
	 */
	if ((zhp = zfs_open(g_zfs, be_container_ds, ZFS_TYPE_FILESYSTEM)) ==
	    NULL) {
		be_print_err(gettext("be_zpool_find_current_be_callback: "
		    "failed to open BE container dataset (%s)\n"),
		    be_container_ds);
		return (0);
	}

	/*
	 * Iterate through all potential BEs in this zpool
	 */
	if (zfs_iter_filesystems(zhp, be_zfs_find_current_be_callback, bt)) {
		/*
		 * Found current BE dataset; set obe_zpool
		 */
		bt->obe_zpool = strdup(zpool);

		zfs_close(zhp);
		return (1);
	}

	zfs_close(zhp);

	return (0);
}

/*
 * Function:	be_zfs_find_current_be_callback
 * Description:	Callback function used to iterate through all BEs in a
 *		pool to find the BE that is the currently booted BE.
 * Parameters:
 *		zhp - zfs_handle_t pointer to current filesystem being checked.
 *		data - be_transaction-data_t pointer
 *			Upon successfully finding the current BE, the
 *			obe_name and obe_root_ds members of this parameter
 *			are set to the BE name and BE's root dataset
 *			respectively.
 * Return:
 *		1 - Found current BE.
 *		0 - Did not find current BE.
 * Scope:
 *		Semi-private (library wide use only)
 */
int
be_zfs_find_current_be_callback(zfs_handle_t *zhp, void *data)
{
	be_transaction_data_t	*bt = data;
	char			*mp = NULL;

	/*
	 * Check if dataset is mounted, and if so where.
	 */
	if (zfs_is_mounted(zhp, &mp)) {
		/*
		 * If mounted at root, set obe_root_ds and obe_name
		 */
		if (mp != NULL && strcmp(mp, "/") == 0) {
			bt->obe_root_ds = strdup(zfs_get_name(zhp));
			bt->obe_name = strdup(basename(bt->obe_root_ds));

			free(mp);

			return (1);
		}

		if (mp != NULL)
			free(mp);
	}

	return (0);
}

/* ********************************************************************	*/
/*			Private Functions				*/
/* ********************************************************************	*/

/*
 * Function:	be_clone_fs_callback
 * Description:	Callback function used to iterate through a BE's filesystems
 *		to clone them for the new BE.
 * Parameters:
 *		zhp - zfs_handle_t pointer for the filesystem being processed.
 *		data - be_transaction_data_t pointer providing information
 *			about original BE and new BE.
 * Return:
 *		0 - Success
 *		non-zero - Failure
 * Scope:
 *		Private
 */
static int
be_clone_fs_callback(zfs_handle_t *zhp, void *data)
{
	be_transaction_data_t	*bt = data;
	zprop_source_t	sourcetype;
	zfs_handle_t	*zhp_ss = NULL;
	char		zhp_name[ZFS_MAXNAMELEN];
	char		ss[MAXPATHLEN];
	char		source[ZFS_MAXNAMELEN];
	char		mountpoint[MAXPATHLEN];
	char		clone_ds[MAXPATHLEN];
	char		*child_fs = NULL;
	int		ret;

	/*
	 * Get a copy of the dataset name zfs_name from zhp
	 */
	(void) strlcpy(zhp_name, zfs_get_name(zhp), sizeof (zhp_name));

	/*
	 * Get dataset name relative to the root.
	 */
	if (strncmp(zhp_name, bt->obe_root_ds, strlen(bt->obe_root_ds))
	    >= 0) {
		child_fs = zhp_name + strlen(bt->obe_root_ds);

		/*
		 * if child_fs is NULL, this means we're processing the
		 * root dataset itself; set child_fs to the empty string.
		 */
		if (child_fs == NULL)
			child_fs = "";
	} else {
		return (1);
	}

	/*
	 * Generate the name of the clone file system.
	 */
	(void) snprintf(clone_ds, sizeof (clone_ds), "%s%s", bt->nbe_root_ds,
	    child_fs);

	/*
	 * Figure out what to set as the mountpoint for this dataset.
	 * If its the root of the BE, then set it to legacy.  Otherwise
	 * grab the mountpoint value from the original dataset.
	 */
	if (strcmp(child_fs, "") == 0) {
		(void) snprintf(mountpoint, sizeof (mountpoint),
		    ZFS_MOUNTPOINT_LEGACY);

		if (nvlist_add_string(bt->nbe_zfs_props,
		    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT), mountpoint) != 0) {
			be_print_err(gettext("be_clone_fs_callback: "
			    "internal error: out of memory\n"));
			return (1);
		}
	} else {
		if (zfs_prop_get(zhp, ZFS_PROP_MOUNTPOINT, mountpoint,
		    sizeof (mountpoint), &sourcetype, source, sizeof (source),
		    B_FALSE) != 0) {
			be_print_err(gettext("be_clone_fs_callback: "
			    "failed to get mountpoint for (%s)\n"), zhp_name);
			return (1);
		}

		/*
		 * If the source of the mountpoint property is local, use the
		 * mountpoint value itself.  Otherwise, remove it from the
		 * zfs properties list so that it get inherited.
		 */
		if (sourcetype & ZPROP_SRC_LOCAL) {
			if (nvlist_add_string(bt->nbe_zfs_props,
			    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT),
			    mountpoint) != 0) {
				be_print_err(gettext("be_clone_fs_callback: "
				    "internal error: out of memory\n"));
				return (1);
			}
		} else {
			ret = nvlist_remove_all(bt->nbe_zfs_props,
			    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT));
			if (ret != 0 && ret != ENOENT) {
				be_print_err(gettext("be_clone_fs_callback: "
				    "failed to remove mountpoint from "
				    "nvlist\n"));
				return (1);
			}
		}
	}

	/*
	 * Set the 'canmount' property
	 */
	if (nvlist_add_string(bt->nbe_zfs_props,
	    zfs_prop_to_name(ZFS_PROP_CANMOUNT), "noauto") != 0) {
		be_print_err(gettext("be_clone_fs_callback: "
		    "internal error: out of memory\n"));
		return (1);
	}

	/*
	 * Generate the name of the snapshot to use.
	 */
	(void) snprintf(ss, sizeof (ss), "%s@%s", zhp_name,
	    bt->obe_snap_name);

	/*
	 * Get handle to snapshot.
	 */
	if ((zhp_ss = zfs_open(g_zfs, ss, ZFS_TYPE_SNAPSHOT)) == NULL) {
		be_print_err(gettext("be_clone_fs_callback: "
		    "failed to get handle to snapshot (%s): %s\n"), ss,
		    libzfs_error_description(g_zfs));
		return (1);
	}

	/*
	 * Clone the dataset.
	 */
	ret = zfs_clone(zhp_ss, clone_ds, bt->nbe_zfs_props);
	if (ret != 0) {
		be_print_err(gettext("be_clone_fs_callback: "
		    "failed to create clone dataset (%s): %s\n"),
		    clone_ds, libzfs_error_description(g_zfs));

		zfs_close(zhp_ss);

		/*
		 * If failed because clone dataset already exists,
		 * return special err code so caller can retry.
		 */
		if (libzfs_errno(g_zfs) == EZFS_EXISTS)
			return (BE_ERR_EXISTS);

		return (1);
	}

	zfs_close(zhp_ss);

	/*
	 * Iterate through zhp's children datasets (if any)
	 * and clone them accordingly.
	 */
	zfs_iter_filesystems(zhp, be_clone_fs_callback, bt);

	return (0);
}

/*
 * Function:	be_send_fs_callback
 * Description: Callback function used to iterate through a BE's filesystems
 *		to copy them for the new BE.
 * Parameters:
 *		zhp - zfs_handle_t pointer for the filesystem being processed.
 *		data - be_transaction_data_t pointer providing information
 *			about original BE and new BE.
 * Return:
 *		0 - Success
 *		non-zero - Failure
 * Scope:
 *		Private
 */
static int
be_send_fs_callback(zfs_handle_t *zhp, void *data)
{
	be_transaction_data_t	*bt = data;
	zprop_source_t	sourcetype;
	recvflags_t	flags = { 0 };
	zfs_handle_t	*zhp_ss = NULL;
	char		zhp_name[ZFS_MAXNAMELEN];
	char		ss[MAXPATHLEN];
	char		source[ZFS_MAXNAMELEN];
	char		mountpoint[MAXPATHLEN];
	char		clone_ds[MAXPATHLEN];
	char		*child_fs = NULL;
	int		pid, status, retval;
	int		srpipe[2];
	int		ret;

	/*
	 * Get a copy of the dataset name  zfs_name from zhp
	 */
	(void) strlcpy(zhp_name, zfs_get_name(zhp), sizeof (zhp_name));

	/*
	 * Get file system name relative to the root.
	 */
	if (strncmp(zhp_name, bt->obe_root_ds, strlen(bt->obe_root_ds))
	    >= 0) {
		child_fs = zhp_name + strlen(bt->obe_root_ds);

		/*
		 * if child_fs is NULL, this means we're processing the
		 * root dataset itself; set child_fs to the empty string.
		 */
		if (child_fs == NULL)
			child_fs = "";
	} else {
		return (1);
	}

	/*
	 * Generate the name of the clone file system.
	 */
	(void) snprintf(clone_ds, sizeof (clone_ds), "%s%s", bt->nbe_root_ds,
	    child_fs);

	/*
	 * Figure out what to set as the mountpoint for this dataset.
	 * If its the root of the BE, then set it to legacy.  Otherwise
	 * grab the mountpoint value from the original dataset.
	 */
	if (strcmp(child_fs, "") == 0) {
		(void) snprintf(mountpoint, sizeof (mountpoint),
		    ZFS_MOUNTPOINT_LEGACY);

		if (nvlist_add_string(bt->nbe_zfs_props,
		    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT), mountpoint) != 0) {
			be_print_err(gettext("be_send_fs_callback: "
			    "internal error: out of memory\n"));
			return (1);
		}
	} else {
		if (zfs_prop_get(zhp, ZFS_PROP_MOUNTPOINT, mountpoint,
		    sizeof (mountpoint), &sourcetype, source, sizeof (source),
		    B_FALSE) != 0) {
			be_print_err(gettext("be_send_fs_callback: "
			    "failed to get mountpoint for (%s)\n"), zhp_name);
			return (1);
		}

		/*
		 * If the source of the mountpoint property is local, use the
		 * mountpoint value itself.  Otherwise, remove it from the
		 * zfs properties list so that it get inherited.
		 */
		if (sourcetype & ZPROP_SRC_LOCAL) {
			if (nvlist_add_string(bt->nbe_zfs_props,
			    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT), mountpoint)
			    != 0) {
				be_print_err(gettext("be_send_fs_callback: "
				    "internal error: out of memory\n"));
				return (1);
			}
		} else {
			ret = nvlist_remove_all(bt->nbe_zfs_props,
			    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT));
			if (ret != 0 && ret != ENOENT) {
				be_print_err(gettext("be_send_fs_callback: "
				    "failed to remove mountpoint from "
				    "nvlist\n"));
				return (1);
			}
		}
	}

	/*
	 * Set the 'canmount' property
	 */
	if (nvlist_add_string(bt->nbe_zfs_props,
	    zfs_prop_to_name(ZFS_PROP_CANMOUNT), "noauto") != 0) {
		return (1);
	}

	/*
	 * Generate the name of the snapshot to use.
	 */
	(void) snprintf(ss, sizeof (ss), "%s@%s", zhp_name,
	    bt->obe_snap_name);

	/*
	 * Get handle to snapshot.
	 */
	if ((zhp_ss = zfs_open(g_zfs, ss, ZFS_TYPE_SNAPSHOT)) == NULL) {
		be_print_err(gettext("be_send_fs_callback: "
		    "failed to get handle to snapshot (%s): %s\n"), ss,
		    libzfs_error_description(g_zfs));
		return (1);
	}

	/*
	 * Create the new  dataset.
	 */
	if (zfs_create(g_zfs, clone_ds, ZFS_TYPE_FILESYSTEM, bt->nbe_zfs_props)
	    != 0) {
		be_print_err(gettext("be_send_fs_callback: "
		    "failed to create new dataset '%s': %s\n"),
		    clone_ds, libzfs_error_description(g_zfs));

		zfs_close(zhp_ss);
		return (1);
	}

	/*
	 * Destination file system is already created
	 * hence we need to set the force flag on
	 */
	flags.force = B_TRUE;

	/*
	 * Initiate the pipe to be used for the send and recv
	 */
	if (pipe(srpipe) != 0) {
		be_print_err(gettext("be_send_fs_callback: failed to "
		    "open pipe\n"));
		return (1);
	}

	/*
	 * Fork off a child to send the dataset
	 */
	if ((pid = fork()) == -1) {
		be_print_err(gettext("be_send_fs_callback: failed to fork\n"));
		(void) close(srpipe[0]);
		(void) close(srpipe[1]);
		return (1);
	} else if (pid == 0) { /* child process */
		(void) close(srpipe[0]);

		/* Send dataset */
		if (zfs_send(zhp, NULL, bt->obe_snap_name, B_FALSE, B_FALSE,
		    B_FALSE, B_FALSE, srpipe[1]) != 0) {
			_exit(1);
		}

		_exit(0);
	}

	(void) close(srpipe[1]);

	/* Receive dataset */
	if (zfs_receive(g_zfs, clone_ds, flags, srpipe[0], NULL) != 0) {
		be_print_err(gettext("be_send_fs_callback: failed to "
		    "recv dataset (%s)\n"), clone_ds);
	}
	(void) close(srpipe[0]);

	/* wait for child to exit */
	do {
		retval = waitpid(pid, &status, 0);
		if (retval == -1) {
			status = 0;
		}
	} while (retval != pid);

	if (WEXITSTATUS(status) != 0) {
		be_print_err(gettext("be_send_fs_callback: failed to "
		    "send dataset (%s)\n"), zhp_name);
		return (1);
	}

	zfs_close(zhp_ss);

	/*
	 * iterate through zhp's children datasets (if any)
	 * and send them accordingly.
	 */
	if (zfs_iter_filesystems(zhp, be_send_fs_callback, bt) != 0) {
		return (1);
	}

	return (0);
}

/*
 * Function:	be_destroy_callback
 * Description:	Callback function used to destroy a BEs children datasets
 *		and snapshots.
 * Parameters:
 *		zhp - zfs_handle_t pointer to the filesystem being processed.
 *		data - Not used.
 * Returns:
 *		0 - Success
 *		1 - Failure
 * Scope:
 *		Private
 */
static int
be_destroy_callback(zfs_handle_t *zhp, void *data)
{
	/* 
	 * Iterate down this file system's heirarchical children
	 * and destroy them first.
	 */
	if (zfs_iter_filesystems(zhp, be_destroy_callback, NULL) != 0)
		return (1);

	/* Attempt to unmount the dataset before destroying it */
	if (zfs_unmount(zhp, NULL, B_TRUE) != 0 || zfs_destroy(zhp) != 0) {
		be_print_err(gettext("be_destroy_callback: failed to "
		    "destroy %s\n"), zfs_get_name(zhp));
		zfs_close(zhp);
		return (1);
	}

	zfs_close(zhp);
	return (0);
}

/*
 * Function:	be_get_snap
 * Description:	This function takes a snapshot dataset name and separates
 *		out the parent dataset portion from the snapshot name.
 *		I.e. it finds the '@' in the snapshot dataset name and
 *		replaces it with a '\0'. 
 * Parameters:
 *		origin - char pointer to a snapshot dataset name.  Its
 *			contents will be modified by this function.
 *		*snap - pointer to a char pointer.  Will be set to the
 *			snapshot name portion upon success.
 * Return:
 *		0 - Success
 *		1 - Failure
 * Scope:
 *		Private
 */
static int
be_get_snap(char *origin, char **snap)
{
	char        *cp;

	/*
	 * Separate out the origin's dataset and snapshot portions by
	 * replacing the @ with a '\0'
	 */
	cp = strrchr(origin, '@');
	if (cp != NULL) {
		if (cp[1] != NULL && cp[1] != '\0') {
			cp[0] = '\0';
			*snap = cp+1;
		} else {
			return (1);
		}
	} else {
		return (1);
	}

	return (0);
}

/*
 * Function:	be_create_container_ds
 * Description:	This function checks that the zpool passed has the BE
 *		container dataset, and if not, then creates it.
 * Parameters:
 *		zpool - name of pool to create BE container dataset in.
 * Return:
 *		B_TRUE - Successfully created BE container dataset, or it
 *			already existed.
 *		B_FALSE - Failed to create container dataset.
 * Scope:
 *		Private
 */
static boolean_t
be_create_container_ds(char *zpool)
{
	nvlist_t	*props = NULL;
	char		be_container_ds[MAXPATHLEN];

	/* Generate string for BE container dataset for this pool */
	be_make_container_ds(zpool, be_container_ds,
	    sizeof (be_container_ds));

	if (!zfs_dataset_exists(g_zfs, be_container_ds, ZFS_TYPE_FILESYSTEM)) {

		if (nvlist_alloc(&props, NV_UNIQUE_NAME, 0) != 0) {
			be_print_err(gettext("be_create_container_ds: "
			    "nvlist_alloc failed\n"));
			return (B_FALSE);
		}

		if (nvlist_add_string(props,
		    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT),
		    ZFS_MOUNTPOINT_LEGACY) != 0) {
			be_print_err(gettext("be_create_container_ds: "
			    "internal error: out of memory\n"));
			nvlist_free(props);
			return (B_FALSE);
		}

		if (nvlist_add_string(props,
		    zfs_prop_to_name(ZFS_PROP_CANMOUNT), "off") != 0) {
			be_print_err(gettext("be_create_container_ds: "
			    "internal error: out of memory\n"));
			nvlist_free(props);
			return (B_FALSE);
		}

		if (zfs_create(g_zfs, be_container_ds, ZFS_TYPE_FILESYSTEM,
		    props) != 0) {
			be_print_err(gettext("be_create_container_ds: "
			    "failed to create container dataset (%s): %s\n"),
			    be_container_ds, libzfs_error_description(g_zfs));
			nvlist_free(props);
			return (B_FALSE);
		}

		nvlist_free(props);
	}

	return (B_TRUE);
}