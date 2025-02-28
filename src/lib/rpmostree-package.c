/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Red Hat, Inc.
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:rpmostree-package
 * @short_description: An object representing a package
 * @include: rpmostree.h
 * @stability: Stable
 *
 * Represents an RPM package.
 */

#include "config.h"

#include "libglnx.h"
#include <stdlib.h>
#include <string.h>

#include "rpmostree-package-priv.h"
#include "rpmostree-shlib-ipc-private.h"

#if BUILDOPT_HAVE_RPMVER
#include <rpm/rpmver.h>
#else
#include "rpmver-private.h"
#endif

typedef GObjectClass RpmOstreePackageClass;

/* Since the class is small, we perform a poor man's polymorphism; RpmOstreePackage objects
 * can be backed by either a sack or a variant. A ref is kept on the appropriate object.*/

struct RpmOstreePackage
{
  GObject parent_instance;
  GVariant *gv_nevra;
  /* deconstructed/cached values */
  char *nevra;
  const char *name;
  const char *evr;
  char *evr_owned;
  const char *arch;
};

G_DEFINE_TYPE (RpmOstreePackage, rpm_ostree_package, G_TYPE_OBJECT)

static void
rpm_ostree_package_finalize (GObject *object)
{
  RpmOstreePackage *pkg = (RpmOstreePackage *)object;
  g_clear_pointer (&pkg->gv_nevra, g_variant_unref);

  g_clear_pointer (&pkg->nevra, g_free);
  g_clear_pointer (&pkg->evr_owned, g_free);

  G_OBJECT_CLASS (rpm_ostree_package_parent_class)->finalize (object);
}

static void
rpm_ostree_package_class_init (RpmOstreePackageClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = rpm_ostree_package_finalize;
}

static void
rpm_ostree_package_init (RpmOstreePackage *p)
{
}

/**
 * rpm_ostree_package_get_nevra:
 * @p: Package
 *
 * Returns: (transfer none): A formatted UTF-8 string containing the name, epoch, version,
 * release, and architecture.  Avoid parsing this; instead use
 * individual getters for more precise control.
 */
const char *
rpm_ostree_package_get_nevra (RpmOstreePackage *p)
{
  return p->nevra;
}

/**
 * rpm_ostree_package_get_name:
 * @p: Package
 *
 * Returns: (transfer none): The package name
 */
const char *
rpm_ostree_package_get_name (RpmOstreePackage *p)
{
  return p->name;
}

/**
 * rpm_ostree_package_get_evr:
 * @p: Package
 *
 * Returns: (transfer none): The package epoch:version-release
 */
const char *
rpm_ostree_package_get_evr (RpmOstreePackage *p)
{
  return p->evr;
}

/**
 * rpm_ostree_package_get_arch:
 * @p: Package
 *
 * Returns: (transfer none): The package architecture
 */
const char *
rpm_ostree_package_get_arch (RpmOstreePackage *p)
{
  return p->arch;
}

/**
 * evr_cmp:
 * @a: String representation of EVR
 * @b: String representation of EVR
 *
 * Compares two string representations of EVR by calling rpmverCmp
 *
 * Returns: 1: a is newer than b
 *          0: a and b are the same version
 *         -1: b is newer than a
 */
static int
evr_cmp (const char *a, const char *b)
{
  rpmver v1 = rpmverParse (a);
  rpmver v2 = rpmverParse (b);
  int rc = rpmverCmp (v1, v2);
  rpmverFree (v1);
  rpmverFree (v2);
  return rc;
}

/**
 * rpm_ostree_package_cmp:
 * @p1: Package
 * @p2: Package
 *
 * Compares two packages by name, epoch:version-release and architecture.
 *
 * Returns: an integer suitable for sorting functions; negative if @p1 should
 *          sort before @p2 in name or version, 0 if equal, positive if @p1
 *          should sort after @p2
 */
int
rpm_ostree_package_cmp (RpmOstreePackage *p1, RpmOstreePackage *p2)
{
  int ret = strcmp (p1->name, p2->name);
  if (ret)
    return ret;

  /* Note we shouldn't hit this case often: the pkglist is already sorted
   * when we read it out of the commit metadata and we also sort
   * the diff in_rpm_ostree_diff_package_lists().
   **/
  ret = evr_cmp (p1->evr, p2->evr);
  if (ret)
    return ret;

  return strcmp (p1->arch, p2->arch);
}

RpmOstreePackage *
_rpm_ostree_package_new_from_variant (GVariant *gv_nevra)
{
  RpmOstreePackage *p = g_object_new (RPM_OSTREE_TYPE_PACKAGE, NULL);
  p->gv_nevra = g_variant_ref (gv_nevra);

  /* we deconstruct now to make accessors simpler */
  const char *epoch;
  const char *version;
  const char *release;

  g_variant_get (p->gv_nevra, "(&s&s&s&s&s)", &p->name, &epoch, &version, &release, &p->arch);
  /* we follow the libdnf convention here of explicit 0 --> skip over */
  g_assert (epoch);
  if (g_str_equal (epoch, "0"))
    p->evr_owned = g_strdup_printf ("%s-%s", version, release);
  else
    p->evr_owned = g_strdup_printf ("%s:%s-%s", epoch, version, release);
  p->evr = p->evr_owned;
  p->nevra = g_strdup_printf ("%s-%s.%s", p->name, p->evr, p->arch);
  return p;
}

static GVariant *
get_commit_rpmdb_pkglist (GVariant *commit)
{
  g_autoptr (GVariant) meta = g_variant_get_child_value (commit, 0);
  g_autoptr (GVariantDict) meta_dict = g_variant_dict_new (meta);
  return g_variant_dict_lookup_value (meta_dict, "rpmostree.rpmdb.pkglist",
                                      G_VARIANT_TYPE ("a(sssss)"));
}

gboolean
_rpm_ostree_package_variant_list_for_commit (OstreeRepo *repo, const char *rev,
                                             gboolean allow_noent, GVariant **out_pkglist,
                                             GCancellable *cancellable, GError **error)
{
  GLNX_AUTO_PREFIX_ERROR ("Loading package list", error);
  g_autofree char *checksum = NULL;
  if (!ostree_repo_resolve_rev (repo, rev, FALSE, &checksum, error))
    return FALSE;

  g_autoptr (GVariant) commit = NULL;
  if (!ostree_repo_load_variant (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum, &commit, error))
    return FALSE;

  /* If there's no commit metadata, we fallback to checking out the rpmdb from the commit
   * using the IPC mechanism. */
  g_autoptr (GVariant) pkglist_v = get_commit_rpmdb_pkglist (commit);
  if (!pkglist_v)
    {
      /* Yeah we could extend the IPC to support sending a fd too but for
       * now communicating via the working directory is easier.
       */
      int fd = ostree_repo_get_dfd (repo);
      g_autofree char *fdpath = g_strdup_printf ("/proc/self/fd/%d", fd);
      char *args[] = { "packagelist-from-commit", (char *)rev, NULL };
      g_autoptr (GVariant) maybe_pkglist_v
          = _rpmostree_shlib_ipc_send ("m" RPMOSTREE_SHLIB_IPC_PKGLIST, args, fdpath, error);
      if (!maybe_pkglist_v)
        return FALSE;
      pkglist_v = g_variant_get_maybe (maybe_pkglist_v);
      if (!pkglist_v)
        {
          if (!allow_noent)
            return glnx_throw (error, "No package database found");
        }
    }
  *out_pkglist = g_steal_pointer (&pkglist_v);
  return TRUE;
}

/* Opportunistically try to use the new rpmostree.rpmdb.pkglist metadata, otherwise fall
 * back to commit rpmdb if available.
 *
 * Let's keep this private for now.
 */
gboolean
_rpm_ostree_package_list_for_commit (OstreeRepo *repo, const char *rev, gboolean allow_noent,
                                     GPtrArray **out_pkglist, GCancellable *cancellable,
                                     GError **error)
{

  g_autoptr (GVariant) pkglist_v = NULL;
  if (!_rpm_ostree_package_variant_list_for_commit (repo, rev, allow_noent, &pkglist_v, cancellable,
                                                    error))
    return FALSE;

  g_autoptr (GPtrArray) pkglist = g_ptr_array_new_with_free_func (g_object_unref);
  const guint n = g_variant_n_children (pkglist_v);
  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GVariant) pkg_v = g_variant_get_child_value (pkglist_v, i);
      g_ptr_array_add (pkglist, _rpm_ostree_package_new_from_variant (pkg_v));
    }

  /* sanity check that we added stuff */
  g_assert_cmpint (pkglist->len, >, 0);

  *out_pkglist = g_steal_pointer (&pkglist);
  return TRUE;
}

static inline gboolean
next_pkg_has_different_name (const char *name, GPtrArray *pkgs, guint cur_i)
{
  if (cur_i + 1 >= pkgs->len)
    return TRUE;
  RpmOstreePackage *pkg = g_ptr_array_index (pkgs, cur_i + 1);
  return !g_str_equal (name, pkg->name);
}

/* Kinda like `comm(1)`, but for RpmOstreePackage lists. Assuming the pkglists are sorted,
 * this is more efficient than launching hundreds of queries and works with both dnf-based
 * and rpmdb.pkglist-based RpmOstreePackage arrays. Packages with different arches (e.g.
 * multilib) are counted as different packages.
 * */
gboolean
_rpm_ostree_diff_package_lists (GPtrArray *a, GPtrArray *b, GPtrArray **out_unique_a,
                                GPtrArray **out_unique_b, GPtrArray **out_modified_a,
                                GPtrArray **out_modified_b, GPtrArray **out_common)
{
  g_assert (a != NULL && b != NULL);
  g_return_val_if_fail (
      out_unique_a || out_unique_b || out_modified_a || out_modified_b || out_common, FALSE);

  const guint an = a->len;
  const guint bn = b->len;

  g_autoptr (GPtrArray) unique_a = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) unique_b = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) modified_a = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) modified_b = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GPtrArray) common = g_ptr_array_new_with_free_func (g_object_unref);

  guint cur_a = 0;
  guint cur_b = 0;
  while (cur_a < an && cur_b < bn)
    {
      int cmp;
      RpmOstreePackage *pkg_a = g_ptr_array_index (a, cur_a);
      RpmOstreePackage *pkg_b = g_ptr_array_index (b, cur_b);

      cmp = strcmp (pkg_a->name, pkg_b->name);
      if (cmp < 0)
        {
          g_ptr_array_add (unique_a, g_object_ref (pkg_a));
          cur_a++;
        }
      else if (cmp > 0)
        {
          g_ptr_array_add (unique_b, g_object_ref (pkg_b));
          cur_b++;
        }
      else
        {
          cmp = strcmp (pkg_a->arch, pkg_b->arch);
          if (cmp == 0)
            {
              cmp = evr_cmp (pkg_a->evr, pkg_b->evr);
              if (cmp == 0)
                {
                  g_ptr_array_add (common, g_object_ref (pkg_a));
                }
              else
                {
                  g_ptr_array_add (modified_a, g_object_ref (pkg_a));
                  g_ptr_array_add (modified_b, g_object_ref (pkg_b));
                }
              cur_a++;
              cur_b++;
            }
          else
            {
              /* if it's just a *single* package of that name that changed arch, let's catch
               * it to match yum/dnf. otherwise (multilib), just report them separately. */
              const gboolean single_a = next_pkg_has_different_name (pkg_a->name, a, cur_a);
              const gboolean single_b = next_pkg_has_different_name (pkg_b->name, b, cur_b);
              if (single_a && single_b)
                {
                  g_ptr_array_add (modified_a, g_object_ref (pkg_a));
                  g_ptr_array_add (modified_b, g_object_ref (pkg_b));
                  cur_a++;
                  cur_b++;
                }
              else if (cmp < 0)
                {
                  g_ptr_array_add (unique_a, g_object_ref (pkg_a));
                  cur_a++;
                }
              else if (cmp > 0)
                {
                  g_ptr_array_add (unique_b, g_object_ref (pkg_b));
                  cur_b++;
                }
            }
        }
    }

  /* flush out remaining a */
  for (; cur_a < an; cur_a++)
    g_ptr_array_add (unique_a, g_object_ref (g_ptr_array_index (a, cur_a)));

  /* flush out remaining b */
  for (; cur_b < bn; cur_b++)
    g_ptr_array_add (unique_b, g_object_ref (g_ptr_array_index (b, cur_b)));

  g_assert_cmpuint (cur_a, ==, an);
  g_assert_cmpuint (cur_b, ==, bn);
  g_assert_cmpuint (modified_a->len, ==, modified_b->len);

  if (out_unique_a)
    *out_unique_a = g_steal_pointer (&unique_a);
  if (out_unique_b)
    *out_unique_b = g_steal_pointer (&unique_b);
  if (out_modified_a)
    *out_modified_a = g_steal_pointer (&modified_a);
  if (out_modified_b)
    *out_modified_b = g_steal_pointer (&modified_b);
  if (out_common)
    *out_common = g_steal_pointer (&common);
  return TRUE;
}
