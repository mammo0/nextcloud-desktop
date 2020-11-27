/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2015-2013 by Klaas Freitag <freitag@owncloud.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <cstdio>

#include "csync.h"
#include "std/c_alloc.h"
#include "std/c_string.h"
#include "vio/csync_vio_local.h"

#include <QDir>

static const auto CSYNC_TEST_DIR = []{ return QStringLiteral("%1/csync_test").arg(QDir::tempPath());}();

#include "torture.h"

namespace {
int oc_mkdir(const QString &path)
{
    return QDir(path).mkpath(path) ? 0 : -1;
}

}
#define WD_BUFFER_SIZE 255

static mbchar_t wd_buffer[WD_BUFFER_SIZE];

typedef struct {
    char  *result;
    char *ignored_dir;
} statevar;

/* remove the complete test dir */
static int wipe_testdir()
{
  QDir tmp(CSYNC_TEST_DIR);
  if (tmp.exists())
  {
      return tmp.removeRecursively() ? 0 : 1;
  }
  return 0;
}

static int setup_testenv(void **state) {
    int rc;

    rc = wipe_testdir();
    assert_int_equal(rc, 0);

    auto dir = CSYNC_TEST_DIR;
    rc = oc_mkdir(dir);
    assert_int_equal(rc, 0);

    assert_non_null(_tgetcwd(wd_buffer, WD_BUFFER_SIZE));

#ifdef Q_OS_WIN
    rc  = _tchdir(dir.toStdWString().data());
#else
    rc  = _tchdir(dir.toLocal8Bit().constData());
#endif
    assert_int_equal(rc, 0);

    /* --- initialize csync */
    statevar *mystate = (statevar*)malloc( sizeof(statevar) );
    mystate->result = NULL;

    *state = mystate;
    return 0;
}

static void output( const char *text )
{
    printf("%s\n", text);
}

static int teardown(void **state) {
    int rc;

    output("================== Tearing down!\n");

    rc = _tchdir(wd_buffer);
    assert_int_equal(rc, 0);

    rc = wipe_testdir();
    assert_int_equal(rc, 0);

    SAFE_FREE(((statevar*)*state)->result);
    SAFE_FREE(*state);
    return 0;
}

/* This function takes a relative path, prepends it with the CSYNC_TEST_DIR
 * and creates each sub directory.
 */
static void create_dirs( const char *path )
{
  int rc;
  auto _mypath = QStringLiteral("%1/%2").arg(CSYNC_TEST_DIR, path).toUtf8();
  char *mypath = _mypath.data();

  char *p = mypath + CSYNC_TEST_DIR.size() + 1; /* start behind the offset */
  int i = 0;

  assert_non_null(path);

  while( *(p+i) ) {
    if( *(p+i) == '/' ) {
      p[i] = '\0';

      auto mb_dir = QString::fromUtf8(mypath);
      rc = oc_mkdir(mb_dir);
      if(rc)
      {
          rc = errno;
      }
      assert_int_equal(rc, 0);
      p[i] = '/';
    }
    i++;
  }
}

/*
 * This function uses the vio_opendir, vio_readdir and vio_closedir functions
 * to traverse a file tree that was created before by the create_dir function.
 *
 * It appends a listing to the result member of the incoming struct in *state
 * that can be compared later to what was expected in the calling functions.
 * 
 * The int parameter cnt contains the number of seen files (not dirs) in the
 * whole tree.
 *
 */
static void traverse_dir(void **state, const QString &dir, int *cnt)
{
    csync_vio_handle_t *dh;
    std::unique_ptr<csync_file_stat_t> dirent;
    statevar *sv = (statevar*) *state;
    char *subdir;
    char *subdir_out;
    int rc;
    int is_dir;

    const char *format_str = "%s %s";

    dh = csync_vio_local_opendir(dir);
    assert_non_null(dh);

    OCC::Vfs *vfs = nullptr;
    while( (dirent = csync_vio_local_readdir(dh, vfs)) ) {
        assert_non_null(dirent.get());
        if (!dirent->original_path.isEmpty()) {
            sv->ignored_dir = c_strdup(dirent->original_path);
            continue;
        }

        assert_false(dirent->path.isEmpty());

        if( c_streq( dirent->path, "..") || c_streq( dirent->path, "." )) {
          continue;
        }

        is_dir = (dirent->type == ItemTypeDirectory) ? 1:0;

        assert_int_not_equal( asprintf( &subdir, "%s/%s", dir.toUtf8().constData(), dirent->path.constData() ), -1 );

        assert_int_not_equal( asprintf( &subdir_out, format_str,
                                        is_dir ? "<DIR>":"     ",
                                        subdir), -1 );

        if( is_dir ) {
            if( !sv->result ) {
                sv->result = c_strdup( subdir_out);
            } else {
                int newlen = 1+strlen(sv->result)+strlen(subdir_out);
                char *tmp = sv->result;
                sv->result = (char*)c_malloc(newlen);
                strcpy( sv->result, tmp);
                SAFE_FREE(tmp);

                strcat( sv->result, subdir_out );
            }
        } else {
            *cnt = *cnt +1;
        }
        output(subdir_out);
        if( is_dir ) {
          traverse_dir( state, subdir, cnt);
        }

        SAFE_FREE(subdir);
        SAFE_FREE(subdir_out);
    }

    rc = csync_vio_local_closedir(dh);
    assert_int_equal(rc, 0);

}

static void create_file( const char *path, const char *name, const char *content)
{
   QFile file(QStringLiteral("%1/%2%3").arg(CSYNC_TEST_DIR, path, name));
   assert_int_equal(1, file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
   file.write(content);
}

static void check_readdir_shorttree(void **state)
{
    statevar *sv = (statevar*) *state;

    const char *t1 = "alibaba/und/die/vierzig/räuber/";
    create_dirs( t1 );
    int files_cnt = 0;
    
    traverse_dir(state, CSYNC_TEST_DIR, &files_cnt);

    assert_string_equal( sv->result,
                         QString::fromUtf8("<DIR> %1/alibaba"
                                        "<DIR> %1/alibaba/und"
                                        "<DIR> %1/alibaba/und/die"
                                        "<DIR> %1/alibaba/und/die/vierzig"
                                        "<DIR> %1/alibaba/und/die/vierzig/räuber").arg(CSYNC_TEST_DIR).toUtf8().constData() );
    assert_int_equal(files_cnt, 0);
}

static void check_readdir_with_content(void **state)
{
    statevar *sv = (statevar*) *state;
    int files_cnt = 0;

    const char *t1 = "warum/nur/40/Räuber/";
    create_dirs( t1 );

    create_file( t1, "Räuber Max.txt", "Der Max ist ein schlimmer finger");
    create_file( t1, "пя́тница.txt", "Am Freitag tanzt der Ürk");


    traverse_dir(state, CSYNC_TEST_DIR, &files_cnt);

    assert_string_equal( sv->result,
                         QString::fromUtf8("<DIR> %1/warum"
                         "<DIR> %1/warum/nur"
                         "<DIR> %1/warum/nur/40"
                         "<DIR> %1/warum/nur/40/Räuber").arg(CSYNC_TEST_DIR).toUtf8().constData());
    /*                   "      %1/warum/nur/40/Räuber/Räuber Max.txt"
                         "      %1/warum/nur/40/Räuber/пя́тница.txt"; */
    assert_int_equal(files_cnt, 2); /* Two files in the sub dir */
}

static void check_readdir_longtree(void **state)
{
    statevar *sv = (statevar*) *state;

    /* Strange things here: Compilers only support strings with length of 4k max.
     * The expected result string is longer, so it needs to be split up in r1, r2 and r3
     */

    /* create the test tree */
    const char *t1 = "vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE/ooooooooooooooooooooooooooohhhhhh/und/BESSER/ZWEI/Butteln/VOLL RUM/";
    create_dirs( t1 );

    const auto r1 = QString::fromUtf8(
"<DIR> %1/vierzig"
"<DIR> %1/vierzig/mann"
"<DIR> %1/vierzig/mann/auf"
"<DIR> %1/vierzig/mann/auf/des"
"<DIR> %1/vierzig/mann/auf/des/toten"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum").arg(CSYNC_TEST_DIR);


    const auto r2 = QString::fromUtf8(
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH").arg(CSYNC_TEST_DIR);


    const auto r3 = QString::fromUtf8(
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE/ooooooooooooooooooooooooooohhhhhh"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE/ooooooooooooooooooooooooooohhhhhh/und"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE/ooooooooooooooooooooooooooohhhhhh/und/BESSER"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE/ooooooooooooooooooooooooooohhhhhh/und/BESSER/ZWEI"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE/ooooooooooooooooooooooooooohhhhhh/und/BESSER/ZWEI/Butteln"
"<DIR> %1/vierzig/mann/auf/des/toten/Mann/kiste/ooooooooooooooooooooooh/and/ne/bottle/voll/rum/und/so/singen/wir/VIERZIG/MANN/AUF/DES/TOTEN/MANNS/KISTE/OOOOOOOOH/AND/NE/BOTTLE/VOLL/RUM/undnochmalallezusammen/VierZig/MannaufDesTotenManns/KISTE/ooooooooooooooooooooooooooohhhhhh/und/BESSER/ZWEI/Butteln/VOLL RUM").arg(CSYNC_TEST_DIR);

    /* assemble the result string ... */
    const auto result = (r1 + r2 + r3).toUtf8();
    int files_cnt = 0;
    traverse_dir(state, CSYNC_TEST_DIR, &files_cnt);
    assert_int_equal(files_cnt, 0);
    /* and compare. */
    assert_string_equal( sv->result, result);


}

// https://github.com/owncloud/client/issues/3128 https://github.com/owncloud/client/issues/2777
static void check_readdir_bigunicode(void **state)
{
    statevar *sv = (statevar*) *state;
//    1: ? ASCII: 239 - EF
//    2: ? ASCII: 187 - BB
//    3: ? ASCII: 191 - BF
//    4: ASCII: 32    - 20

    QString p = QStringLiteral("%1/%2").arg(CSYNC_TEST_DIR, "goodone/" );
    int rc = oc_mkdir(p);
    assert_int_equal(rc, 0);

    p = QStringLiteral("%1/%2").arg(CSYNC_TEST_DIR, "goodone/ugly\xEF\xBB\xBF\x32" ".txt" ); // file with encoding error

    rc = oc_mkdir(p);

    assert_int_equal(rc, 0);

    int files_cnt = 0;
    traverse_dir(state, CSYNC_TEST_DIR, &files_cnt);
    const auto expected_result =  QString::fromUtf8("<DIR> %1/goodone"
                                   "<DIR> %1/goodone/ugly\xEF\xBB\xBF\x32" ".txt").arg(CSYNC_TEST_DIR)
                                   ;
    assert_string_equal( sv->result, expected_result.toUtf8().constData());

    assert_int_equal(files_cnt, 0);
}

int torture_run_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(check_readdir_shorttree, setup_testenv, teardown),
        cmocka_unit_test_setup_teardown(check_readdir_with_content, setup_testenv, teardown),
        cmocka_unit_test_setup_teardown(check_readdir_longtree, setup_testenv, teardown),
        cmocka_unit_test_setup_teardown(check_readdir_bigunicode, setup_testenv, teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
