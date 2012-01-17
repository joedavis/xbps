/*-
 * Copyright (c) 2009-2012 Juan Romero Pardines.
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

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "xbps_api_impl.h"

/**
 * @file lib/repository_pool_find.c
 * @brief Repository pool routines
 * @defgroup repopool Repository pool functions
 */
struct repo_pool_fpkg {
	prop_dictionary_t pkgd;
	const char *pattern;
	const char *bestpkgver;
	const char *repo_bestmatch;
	bool bypattern;
	bool exact;
};

static int
repo_find_virtualpkg_cb(struct repository_pool_index *rpi, void *arg, bool *done)
{
	struct repo_pool_fpkg *rpf = arg;

	assert(rpi != NULL);

	if (rpf->bypattern) {
		rpf->pkgd =
		    xbps_find_virtualpkg_conf_in_dict_by_pattern(rpi->rpi_repod,
		    "packages", rpf->pattern);
	} else {
		rpf->pkgd =
		    xbps_find_virtualpkg_conf_in_dict_by_name(rpi->rpi_repod,
		    "packages", rpf->pattern);
	}
	if (rpf->pkgd) {
#ifdef DEBUG
		xbps_dbg_printf("%s: found pkg in repository\n", __func__);
		xbps_dbg_printf_append("%s", prop_dictionary_externalize(rpf->pkgd));
#endif
		prop_dictionary_set_cstring(rpf->pkgd, "repository",
		    rpi->rpi_uri);
		*done = true;
		return 0;
	}
	/* not found */
	return 0;
}

static int
repo_find_pkg_cb(struct repository_pool_index *rpi, void *arg, bool *done)
{
	struct repo_pool_fpkg *rpf = arg;

	assert(rpi != NULL);

	if (rpf->exact) {
		if (rpf->repo_bestmatch != NULL) {
			if (strcmp(rpf->repo_bestmatch, rpi->rpi_uri))
				return 0;
		}
		/* exact match by pkgver */
		rpf->pkgd = xbps_find_pkg_in_dict_by_pkgver(rpi->rpi_repod,
		    "packages", rpf->pattern);
	} else if (rpf->bypattern) {
		/* match by pkgpattern in pkgver*/
		rpf->pkgd = xbps_find_pkg_in_dict_by_pattern(rpi->rpi_repod,
		    "packages", rpf->pattern);
		/* If no pkg exists matching pattern, look for virtual packages */
		if (rpf->pkgd == NULL) {
			rpf->pkgd = xbps_find_virtualpkg_in_dict_by_pattern(
			    rpi->rpi_repod, "packages", rpf->pattern);
		}
	} else {
		/* match by pkgname */
		rpf->pkgd = xbps_find_pkg_in_dict_by_name(rpi->rpi_repod,
		    "packages", rpf->pattern);
		/* If no pkg exists matching pattern, look for virtual packages */
		if (rpf->pkgd == NULL) {
			rpf->pkgd = xbps_find_virtualpkg_in_dict_by_name(
			    rpi->rpi_repod, "packages", rpf->pattern);
		}
	}
	if (rpf->pkgd) {
		/*
		 * Package dictionary found, add the "repository"
		 * object with the URI.
		 */
		prop_dictionary_set_cstring(rpf->pkgd, "repository",
		    rpi->rpi_uri);
		*done = true;
		return 0;
	}
	/* Not found */
	return 0;
}

static int
repo_find_best_pkg_cb(struct repository_pool_index *rpi,
		      void *arg,
		      bool *done)
{
	struct repo_pool_fpkg *rpf = arg;
	const char *repopkgver;

	assert(rpi != NULL);

	(void)done;

	if (rpf->bypattern) {
		rpf->pkgd = xbps_find_pkg_in_dict_by_pattern(rpi->rpi_repod,
		    "packages", rpf->pattern);
	} else {
		rpf->pkgd = xbps_find_pkg_in_dict_by_name(rpi->rpi_repod,
		    "packages", rpf->pattern);
	}
	if (rpf->pkgd == NULL) {
		if (errno && errno != ENOENT)
			return errno;

		xbps_dbg_printf("[rpool] Package '%s' not found in repository "
		    "'%s'.\n", rpf->pattern, rpi->rpi_uri);
		return 0;
	}
	prop_dictionary_get_cstring_nocopy(rpf->pkgd,
	    "pkgver", &repopkgver);
	if (rpf->bestpkgver == NULL) {
		xbps_dbg_printf("[rpool] Found best match '%s' (%s).\n",
		    repopkgver, rpi->rpi_uri);
		rpf->bestpkgver = repopkgver;
		rpf->repo_bestmatch = rpi->rpi_uri;
		return 0;
	}
	/*
	 * Compare current stored version against new
	 * version from current package in repository.
	 */
	if (xbps_cmpver(repopkgver, rpf->bestpkgver)) {
		xbps_dbg_printf("[rpool] Found best match '%s' (%s).\n",
		    repopkgver, rpi->rpi_uri);
		rpf->bestpkgver = repopkgver;
		rpf->repo_bestmatch = rpi->rpi_uri;
	}
	return 0;
}

static struct repo_pool_fpkg *
repo_find_pkg(const char *pkg, bool bypattern, bool best, bool exact,
	      bool virtual)
{
	struct repo_pool_fpkg *rpf;
	int rv = 0;

	assert(pkg != NULL);

	rpf = malloc(sizeof(*rpf));
	if (rpf == NULL)
		return NULL;

	rpf->pattern = pkg;
	rpf->bypattern = bypattern;
	rpf->exact = exact;
	rpf->pkgd = NULL;
	rpf->bestpkgver = NULL;
	rpf->repo_bestmatch = NULL;

	if (exact) {
		/*
		 * Look for exact package match with pkgver in all repos.
		 */
		rv = xbps_repository_pool_foreach(repo_find_pkg_cb, rpf);
		if (rv != 0)
			errno = rv;
	} else if (best) {
		/*
		 * Look for the best package version of a package name or
		 * pattern in all repositories.
		 */
		rv = xbps_repository_pool_foreach(repo_find_best_pkg_cb, rpf);
		if (rv != 0)
			errno = rv;

		if (rpf->bestpkgver != NULL)
			rpf->pkgd =
			    xbps_repository_pool_find_pkg_exact(rpf->bestpkgver);
	} else {
		if (virtual) {
			/*
			 * No package found. Look for virtual package
			 * set by the user or any virtual pkg available.
			 */
			rv = xbps_repository_pool_foreach(repo_find_virtualpkg_cb, rpf);
			if (rv != 0)
				errno = rv;
		} else {
			/*
			 * Look for a package (non virtual) in repositories.
			 */
			rv = xbps_repository_pool_foreach(repo_find_pkg_cb, rpf);
			if (rv != 0)
				errno = rv;
		}
	}

	return rpf;
}

prop_dictionary_t
xbps_repository_pool_find_virtualpkg(const char *pkg, bool bypattern, bool best)
{
	struct repo_pool_fpkg *rpf;
	prop_dictionary_t pkgd = NULL;

	assert(pkg != NULL);

	rpf = repo_find_pkg(pkg, bypattern, best, false, true);
	if (prop_object_type(rpf->pkgd) == PROP_TYPE_DICTIONARY)
		pkgd = prop_dictionary_copy(rpf->pkgd);
	free(rpf);

	return pkgd;
}

prop_dictionary_t
xbps_repository_pool_find_pkg(const char *pkg, bool bypattern, bool best)
{
	struct repo_pool_fpkg *rpf;
	prop_dictionary_t pkgd = NULL;

	assert(pkg != NULL);

	rpf = repo_find_pkg(pkg, bypattern, best, false, false);
	if (prop_object_type(rpf->pkgd) == PROP_TYPE_DICTIONARY)
		pkgd = prop_dictionary_copy(rpf->pkgd);
	free(rpf);

	return pkgd;
}

prop_dictionary_t
xbps_repository_pool_find_pkg_exact(const char *pkgver)
{
	struct repo_pool_fpkg *rpf;
	prop_dictionary_t pkgd = NULL;

	assert(pkgver != NULL);

	rpf = repo_find_pkg(pkgver, false, false, true, false);
	if (prop_object_type(rpf->pkgd) == PROP_TYPE_DICTIONARY)
		pkgd = prop_dictionary_copy(rpf->pkgd);
	free(rpf);

	return pkgd;
}

prop_dictionary_t
xbps_repository_pool_dictionary_metadata_plist(const char *pkgname,
					       const char *plistf)
{
	prop_dictionary_t pkgd = NULL, plistd = NULL;
	const char *repoloc;
	char *url;

	assert(pkgname != NULL);
	assert(plistf != NULL);

	/*
	 * Iterate over the the repository pool and search for a plist file
	 * in the binary package named 'pkgname'. The plist file will be
	 * internalized to a proplib dictionary.
	 *
	 * The first repository that has it wins and the loop is stopped.
	 * This will work locally and remotely, thanks to libarchive and
	 * libfetch!
	 */
	pkgd = xbps_repository_pool_find_pkg(pkgname, false, false);
	if (pkgd == NULL)
		goto out;

	prop_dictionary_get_cstring_nocopy(pkgd, "repository", &repoloc);
	url = xbps_path_from_repository_uri(pkgd, repoloc);
	if (url == NULL) {
		errno = EINVAL;
		goto out;
	}
	plistd = xbps_dictionary_metadata_plist_by_url(url, plistf);
	free(url);

out:
	if (plistd == NULL)
		errno = ENOENT;
	if (prop_object_type(pkgd) == PROP_TYPE_DICTIONARY)
		prop_object_release(pkgd);

	return plistd;
}
