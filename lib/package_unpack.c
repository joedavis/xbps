/*-
 * Copyright (c) 2008-2012 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/stat.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>

#include "xbps_api_impl.h"

/**
 * @file lib/package_unpack.c
 * @brief Binary package file unpacking routines
 * @defgroup unpack Binary package file unpacking functions
 *
 * Unpacking a binary package involves the following steps:
 *  - Its <b>pre-install</b> target in the INSTALL script is executed
 *    (if available).
 *  - Metadata files are extracted.
 *  - All other kind of files on archive are extracted.
 *  - Handles configuration files by taking care of updating them with
 *    new versions if necessary and to not overwrite modified ones.
 *  - Files from installed package are compared with new package and
 *    obsolete files are removed.
 *  - Finally its state is set to XBPS_PKG_STATE_UNPACKED.
 *
 * The following image shown below represents a transaction dictionary
 * returned by xbps_transaction_prepare():
 *
 * @image html images/xbps_transaction_dictionary.png
 *
 * Legend:
 *   - <b>Salmon filled box</b>: The transaction dictionary.
 *   - <b>White filled box</b>: mandatory objects.
 *   - <b>Grey filled box</b>: optional objects.
 *   - <b>Green filled box</b>: possible value set in the object, only one of
 *     them is set.
 *
 * Text inside of white boxes are the key associated with the object, its
 * data type is specified on its edge, i.e string, array, integer, dictionary.
 */

static int
set_extract_flags(void)
{
	int flags;

	if (geteuid() == 0)
		flags = FEXTRACT_FLAGS;
	else
		flags = EXTRACT_FLAGS;

	return flags;
}

static int
extract_metafile(struct archive *ar,
		 struct archive_entry *entry,
		 const char *file,
		 const char *pkgver,
		 bool exec,
		 int flags)
{
	const char *version;
	char *buf, *dirc, *dname, *pkgname;
	int rv;

	pkgname = xbps_pkg_name(pkgver);
	if (pkgname == NULL)
		return ENOMEM;
	version = xbps_pkg_version(pkgver);
	if (version == NULL) {
		free(pkgname);
		return ENOMEM;
	}
	buf = xbps_xasprintf("%s/metadata/%s/%s",
	    XBPS_META_PATH, pkgname, file);
	if (buf == NULL) {
		free(pkgname);
		return ENOMEM;
	}
	archive_entry_set_pathname(entry, buf);
	dirc = strdup(buf);
	if (dirc == NULL) {
		free(buf);
		free(pkgname);
		return ENOMEM;
	}
	free(buf);
	dname = dirname(dirc);
	if (access(dname, X_OK) == -1) {
		if (xbps_mkpath(dname, 0755) == -1) {
			xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
			    errno, pkgname, version,
			    "%s: [unpack] failed to create metadir `%s': %s",
			    pkgver, dname, strerror(errno));
			free(dirc);
			free(pkgname);
			return errno;

		}
	}
	if (exec)
		archive_entry_set_perm(entry, 0750);

	if (archive_read_extract(ar, entry, flags) != 0) {
		rv = archive_errno(ar);
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    rv, pkgname, version,
		    "%s: [unpack] failed to extract metafile `%s': %s",
		    pkgver, file, strerror(rv));
		free(pkgname);
		return rv;
	}
	free(pkgname);

	return 0;
}

static int
remove_metafile(const char *file, const char *pkgver)
{
	const char *version;
	char *buf, *pkgname;

	pkgname = xbps_pkg_name(pkgver);
	if (pkgname == NULL)
		return ENOMEM;
	version = xbps_pkg_version(pkgver);
	if (version == NULL) {
		free(pkgname);
		return ENOMEM;
	}
	buf = xbps_xasprintf("%s/metadata/%s/%s",
	    XBPS_META_PATH, pkgname, file);
	if (buf == NULL) {
		free(pkgname);
		return ENOMEM;
	}
	if (unlink(buf) == -1) {
		if (errno && errno != ENOENT) {
			xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
			    errno, pkgname, version,
			    "%s: [unpack] failed to remove metafile `%s': %s",
			    pkgver, file, strerror(errno));
			free(pkgname);
			free(buf);
			return errno;
		}
	}
	free(buf);
	free(pkgname);

	return 0;
}

static int
unpack_archive(prop_dictionary_t pkg_repod, struct archive *ar)
{
	prop_dictionary_t propsd = NULL, filesd = NULL, old_filesd = NULL;
	prop_array_t array;
	const struct xbps_handle *xhp = xbps_handle_get();
	const struct stat *entry_statp;
	struct xbps_unpack_cb_data *xucd = NULL;
	struct archive_entry *entry;
	size_t nmetadata = 0, entry_idx = 0;
	const char *entry_pname, *transact, *pkgname, *version, *pkgver, *fname;
	char *buf = NULL, *pkgfilesd = NULL;
	int rv, flags;
	bool preserve, update, conf_file, file_exists;

	assert(prop_object_type(pkg_repod) == PROP_TYPE_DICTIONARY);
	assert(ar != NULL);

	preserve = update = conf_file = file_exists = false;
	prop_dictionary_get_bool(pkg_repod, "preserve", &preserve);
	prop_dictionary_get_cstring_nocopy(pkg_repod,
	    "transaction", &transact);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "pkgname", &pkgname);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "version", &version);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "pkgver", &pkgver);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "filename", &fname);

	if (xhp->unpack_cb != NULL) {
		/* initialize data for unpack cb */
		xucd = malloc(sizeof(*xucd));
		if (xucd == NULL)
			return ENOMEM;

		xucd->entry_extract_count = 0;
		xucd->entry_total_count = 0;
	}
	if (access(xhp->rootdir, R_OK) == -1) {
		if (errno != ENOENT) {
			rv = errno;
			goto out;
		}
		if (xbps_mkpath(xhp->rootdir, 0750) == -1) {
			rv = errno;
			goto out;
		}
	}
	if (chdir(xhp->rootdir) == -1) {
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    errno, pkgname, version,
		    "%s: [unpack] failed to chdir to rootdir `%s': %s",
		    pkgver, xhp->rootdir, strerror(errno));
		rv = errno;
		goto out;
	}

	if (strcmp(transact, "update") == 0)
		update = true;

	/*
	 * While updating, always remove current INSTALL/REMOVE
	 * scripts, because a package upgrade might not have those
	 * anymore.
	 */
	if (update) {
		if ((rv = remove_metafile("INSTALL", pkgver)) != 0)
			goto out;
		if ((rv = remove_metafile("REMOVE", pkgver)) != 0)
			goto out;
	}
	/*
	 * Process the archive files.
	 */
	while (archive_read_next_header(ar, &entry) == ARCHIVE_OK) {
		entry_statp = archive_entry_stat(entry);
		entry_pname = archive_entry_pathname(entry);
		flags = set_extract_flags();
		/*
		 * Ignore directories from archive.
		 */
		if (S_ISDIR(entry_statp->st_mode)) {
			archive_read_data_skip(ar);
			continue;
		}
		/*
		 * Prepare unpack callback ops.
		 */
		if (xucd != NULL) {
			xucd->entry = entry_pname;
			xucd->entry_size = archive_entry_size(entry);
			xucd->entry_is_metadata = false;
			xucd->entry_is_conf = false;
		}
		if (strcmp("./INSTALL", entry_pname) == 0) {
			/*
			 * Extract the INSTALL script first to execute
			 * the pre install target.
			 */
			buf = xbps_xasprintf("%s/metadata/%s/INSTALL",
			    XBPS_META_PATH, pkgname);
			if (buf == NULL) {
				rv = ENOMEM;
				goto out;
			}
			rv = extract_metafile(ar, entry, "INSTALL",
			    pkgver, true, flags);
			if (rv != 0)
				goto out;

			rv = xbps_file_exec(buf, "pre",
			     pkgname, version, update ? "yes" : "no",
			     xhp->conffile, NULL);
			free(buf);
			buf = NULL;
			if (rv != 0) {
				xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
				    rv, pkgname, version,
				    "%s: [unpack] INSTALL script failed "
				    "to execute pre ACTION: %s",
				    pkgver, strerror(rv));
				goto out;
			}
			nmetadata++;
			if (xucd != NULL) {
				xucd->entry_is_metadata = true;
				xucd->entry_extract_count++;
				(*xhp->unpack_cb)(xucd, xhp->unpack_cb_data);
			}
			continue;

		} else if (strcmp("./REMOVE", entry_pname) == 0) {
			rv = extract_metafile(ar, entry, "REMOVE",
			    pkgver, true, flags);
			if (rv != 0)
				goto out;

			nmetadata++;
			if (xucd != NULL) {
				xucd->entry_is_metadata = true;
				xucd->entry_extract_count++;
				(*xhp->unpack_cb)(xucd, xhp->unpack_cb_data);
			}
			continue;

		} else if (strcmp("./files.plist", entry_pname) == 0) {
			/*
			 * Internalize this entry into a prop_dictionary
			 * to check for obsolete files if updating a package.
			 * It will be extracted to disk at the end.
			 */
			filesd = xbps_dictionary_from_archive_entry(ar, entry);
			if (filesd == NULL) {
				rv = errno;
				goto out;
			}
			nmetadata++;
			if (xucd != NULL) {
				xucd->entry_is_metadata = true;
				xucd->entry_extract_count++;
				(*xhp->unpack_cb)(xucd, xhp->unpack_cb_data);
			}
			continue;

		} else if (strcmp("./props.plist", entry_pname) == 0) {
			rv = extract_metafile(ar, entry, XBPS_PKGPROPS,
			    pkgver, false, flags);
			if (rv != 0)
				goto out;

			propsd = xbps_dictionary_from_metadata_plist(
			    pkgname, XBPS_PKGPROPS);
			if (propsd == NULL) {
				rv = errno;
				goto out;
			}
			nmetadata++;
			if (xucd != NULL) {
				xucd->entry_is_metadata = true;
				xucd->entry_extract_count++;
				(*xhp->unpack_cb)(xucd, xhp->unpack_cb_data);
			}
			continue;
		}
		/*
		 * If XBPS_PKGFILES or XBPS_PKGPROPS weren't found
		 * in the archive at this phase, skip all data.
		 */
		if (propsd == NULL || filesd == NULL) {
			archive_read_data_skip(ar);
			/*
			 * If we have processed 4 entries and the two
			 * required metadata files weren't found, bail out.
			 * This is not an XBPS binary package.
			 */
			if (entry_idx >= 3) {
				xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
				    ENODEV, pkgname, version,
				    "%s: [unpack] invalid binary package `%s'.",
				    pkgver, fname);
				rv = ENODEV;
				goto out;
			}

			entry_idx++;
			continue;
		}
		/*
		 * Compute total entries in progress data, if set.
		 * total_entries = metadata + files + conf_files + links.
		 */
		if (xucd != NULL) {
			xucd->entry_total_count = nmetadata;
			array = prop_dictionary_get(filesd, "files");
			xucd->entry_total_count +=
			    (ssize_t)prop_array_count(array);
			array = prop_dictionary_get(filesd, "conf_files");
			xucd->entry_total_count +=
			    (ssize_t)prop_array_count(array);
			array = prop_dictionary_get(filesd, "links");
			xucd->entry_total_count +=
			    (ssize_t)prop_array_count(array);
		}
		/*
		 * Always check that extracted file exists and hash
		 * doesn't match, in that case overwrite the file.
		 * Otherwise skip extracting it.
		 */
		conf_file = false;
		file_exists = false;
		if (S_ISREG(entry_statp->st_mode)) {
			if (xbps_entry_is_a_conf_file(propsd, entry_pname))
				conf_file = true;
			if (access(entry_pname, R_OK) == 0) {
				file_exists = true;
				rv = xbps_file_hash_check_dictionary(filesd,
				    conf_file ? "conf_files" : "files",
				    entry_pname);
				if (rv == -1) {
					xbps_dbg_printf("%s-%s: failed to check"
					    " hash for `%s': %s\n", pkgname,
					    version, entry_pname,
					    strerror(errno));
					/* error */
					goto out;
				} else if (rv == 0) {
					/* hash match, skip */
					xbps_dbg_printf("%s-%s: entry %s "
					    "matches current SHA256, "
					    "skipping...\n", pkgname,
					    version, entry_pname);
					archive_read_data_skip(ar);
					continue;
				}
				rv = 0;
			}
		}
		if (!update && conf_file && file_exists) {
			/*
			 * If installing new package preserve old configuration
			 * file but renaming it to <file>.old.
			 */
			buf = xbps_xasprintf("%s.old", entry_pname);
			if (buf == NULL) {
				rv = ENOMEM;
				goto out;
			}
			(void)rename(entry_pname, buf);
			free(buf);
			buf = NULL;
			xbps_set_cb_state(XBPS_STATE_CONFIG_FILE, 0,
			    pkgname, version,
			    "Renamed old configuration file "
			    "`%s' to `%s.old'.", entry_pname, entry_pname);
		} else if (update && conf_file && file_exists) {
			/*
			 * Handle configuration files. Check if current entry is
			 * a configuration file and take action if required. Skip
			 * packages that don't have the "conf_files" array in
			 * the XBPS_PKGPROPS dictionary.
			 */
			if (xucd != NULL)
				xucd->entry_is_conf = true;

			rv = xbps_entry_install_conf_file(filesd,
			    entry, entry_pname, pkgname, version);
			if (rv == -1) {
				/* error */
				goto out;
			} else if (rv == 0) {
				/*
				 * Keep current configuration file
				 * as is now and pass to next entry.
				 */
				archive_read_data_skip(ar);
				continue;
			}
			rv = 0;
		}
		/*
		 * Extract entry from archive.
		 */
		if (archive_read_extract(ar, entry, flags) != 0) {
			rv = archive_errno(ar);
			xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
			    rv, pkgname, version,
			    "%s: [unpack] failed to extract file `%s': %s",
			    pkgver, entry_pname, strerror(rv));
		}
		if (xucd != NULL) {
			xucd->entry_extract_count++;
			(*xhp->unpack_cb)(xucd, xhp->unpack_cb_data);
		}
	}
	/*
	 * If there was any error extracting files from archive, error out.
	 */
	if ((rv = archive_errno(ar)) != 0) {
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    rv, pkgname, version,
		    "%s: [unpack] error while extracting files from `%s': %s",
		    pkgver, fname, strerror(rv));
		goto out;
	}
	/*
	 * On pkgs that set the preserve keyword or while installing
	 * new packages, do not check for obsolete files.
	 */
	pkgfilesd = xbps_xasprintf("%s/metadata/%s/%s",
	    XBPS_META_PATH, pkgname, XBPS_PKGFILES);
	if (pkgfilesd == NULL) {
		rv = ENOMEM;
		goto out;
	}
	if (preserve || !update)
		goto out1;
	/*
	 * Check for obsolete files.
	 */
	old_filesd = prop_dictionary_internalize_from_zfile(pkgfilesd);
	if (prop_object_type(old_filesd) == PROP_TYPE_DICTIONARY) {
		rv = xbps_remove_obsoletes(pkgname, version,
		    pkgver, old_filesd, filesd);
		prop_object_release(old_filesd);
		if (rv != 0) {
			rv = errno;
			goto out;
		}
	} else if (errno && errno != ENOENT) {
		rv = errno;
		goto out;
	}
out1:
	/*
	 * Create pkg metadata directory.
	 */
	buf = xbps_xasprintf("%s/metadata/%s", XBPS_META_PATH, pkgname);
	if (buf == NULL) {
		rv = ENOMEM;
		goto out;
	}
	if (xbps_mkpath(buf, 0755) == -1) {
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    errno, pkgname, version,
		    "%s: [unpack] failed to create pkg metadir `%s': %s",
		    buf, pkgver, strerror(errno));
		rv = errno;
		goto out;
	}
	/*
	 * Externalize XBPS_PKGFILES into pkg metadata directory.
	 */
	if (!prop_dictionary_externalize_to_zfile(filesd, pkgfilesd)) {
		rv = errno;
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    errno, pkgname, version,
		    "%s: [unpack] failed to extract metadata file `%s': %s",
		    pkgver, XBPS_PKGFILES, strerror(errno));
		goto out;
	}
out:
	if (xucd != NULL)
		free(xucd);
	if (pkgfilesd != NULL)
		free(pkgfilesd);
	if (buf != NULL)
		free(buf);
	if (prop_object_type(filesd) == PROP_TYPE_DICTIONARY)
		prop_object_release(filesd);
	if (prop_object_type(propsd) == PROP_TYPE_DICTIONARY)
		prop_object_release(propsd);

	return rv;
}

int HIDDEN
xbps_unpack_binary_pkg(prop_dictionary_t pkg_repod)
{
	struct archive *ar = NULL;
	const char *pkgname, *version, *repoloc, *pkgver, *fname;
	char *bpkg = NULL;
	int rv = 0;

	assert(prop_object_type(pkg_repod) == PROP_TYPE_DICTIONARY);

	prop_dictionary_get_cstring_nocopy(pkg_repod, "pkgname", &pkgname);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "version", &version);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "pkgver", &pkgver);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "repository", &repoloc);
	prop_dictionary_get_cstring_nocopy(pkg_repod, "filename", &fname);

	xbps_set_cb_state(XBPS_STATE_UNPACK, 0, pkgname, version, NULL);

	bpkg = xbps_path_from_repository_uri(pkg_repod, repoloc);
	if (bpkg == NULL) {
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    errno, pkgname, version,
		    "%s: [unpack] cannot determine binary package "
		    "file for `%s': %s", pkgver, fname, strerror(errno));
		return errno;
	}

	if ((ar = archive_read_new()) == NULL) {
		free(bpkg);
		return ENOMEM;
	}
	/*
	 * Enable support for tar format and all compression methods.
	 */
	archive_read_support_compression_all(ar);
	archive_read_support_format_tar(ar);

	if (archive_read_open_filename(ar, bpkg, ARCHIVE_READ_BLOCKSIZE) != 0) {
		rv = archive_errno(ar);
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    rv, pkgname, version,
		    "%s: [unpack] failed to open binary package `%s': %s",
		    pkgver, fname, strerror(rv));
		free(bpkg);
		archive_read_finish(ar);
		return rv;
	}
	free(bpkg);

	/*
	 * Set package state to half-unpacked.
	 */
	if ((rv = xbps_set_pkg_state_installed(pkgname, version, pkgver,
	    XBPS_PKG_STATE_HALF_UNPACKED)) != 0) {
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    rv, pkgname, version,
		    "%s: [unpack] failed to set state to half-unpacked: %s",
		    pkgver, strerror(rv));
		goto out;
	}
	/*
	 * Extract archive files.
	 */
	rv = unpack_archive(pkg_repod, ar);
	if (rv != 0) {
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    rv, pkgname, version,
		    "%s: [unpack] failed to unpack files from archive: %s",
		    pkgver, strerror(rv));
		goto out;
	}
	/*
	 * Set package state to unpacked.
	 */
	if ((rv = xbps_set_pkg_state_installed(pkgname, version, pkgver,
	    XBPS_PKG_STATE_UNPACKED)) != 0) {
		xbps_set_cb_state(XBPS_STATE_UNPACK_FAIL,
		    rv, pkgname, version,
		    "%s: [unpack] failed to set state to unpacked: %s",
		    pkgver, strerror(rv));
	}
out:
	if (ar)
		archive_read_finish(ar);

	return rv;
}
