/* gpgtar-create.c - Create a TAR archive
 * Copyright (C) 2010 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
/* #ifdef HAVE_W32_SYSTEM */
/* # define WIN32_LEAN_AND_MEAN */
/* # include <windows.h> */
/* #else /\*!HAVE_W32_SYSTEM*\/ */
# include <unistd.h>
# include <pwd.h>
# include <grp.h>
/* #endif /\*!HAVE_W32_SYSTEM*\/ */
#include <assert.h>

#include "i18n.h"
#include "../common/sysutils.h"
#include "gpgtar.h"

#ifndef HAVE_LSTAT
#define lstat(a,b) stat ((a), (b))
#endif


/* Object to control the file scanning.  */
struct scanctrl_s;
typedef struct scanctrl_s *scanctrl_t;
struct scanctrl_s
{
  tar_header_t flist;
  tar_header_t *flist_tail;
  int nestlevel;
};




/* Given a fresh header object HDR with only the name field set, try
   to gather all available info.  */
static gpg_error_t
fillup_entry (tar_header_t hdr)
{
  gpg_error_t err;
  struct stat sbuf;

  if (lstat (hdr->name, &sbuf))
    {
      err = gpg_error_from_syserror ();
      log_error ("error stat-ing `%s': %s\n", hdr->name, gpg_strerror (err));
      return err;
    }
  
  if (S_ISREG (sbuf.st_mode))
    hdr->typeflag = TF_REGULAR;
  else if (S_ISDIR (sbuf.st_mode))
    hdr->typeflag = TF_DIRECTORY;
  else if (S_ISCHR (sbuf.st_mode))
    hdr->typeflag = TF_CHARDEV;
  else if (S_ISBLK (sbuf.st_mode))
    hdr->typeflag = TF_BLOCKDEV;
  else if (S_ISFIFO (sbuf.st_mode))
    hdr->typeflag = TF_FIFO;
  else if (S_ISLNK (sbuf.st_mode))
    hdr->typeflag = TF_SYMLINK;
  else 
    hdr->typeflag = TF_NOTSUP;

  /* FIXME: Save DEV and INO? */

  /* Set the USTAR defined mode bits using the system macros.  */
  if (sbuf.st_mode & S_IRUSR)
    hdr->mode |= 0400;
  if (sbuf.st_mode & S_IWUSR)
    hdr->mode |= 0200;
  if (sbuf.st_mode & S_IXUSR)
    hdr->mode |= 0100;
  if (sbuf.st_mode & S_IRGRP)
    hdr->mode |= 0040;
  if (sbuf.st_mode & S_IWGRP)
    hdr->mode |= 0020;
  if (sbuf.st_mode & S_IXGRP)
    hdr->mode |= 0010;
  if (sbuf.st_mode & S_IROTH)
    hdr->mode |= 0004;
  if (sbuf.st_mode & S_IWOTH)
    hdr->mode |= 0002;
  if (sbuf.st_mode & S_IXOTH)
    hdr->mode |= 0001;
#ifdef S_IXUID
  if (sbuf.st_mode & S_IXUID)
    hdr->mode |= 04000;
#endif
#ifdef S_IXGID
  if (sbuf.st_mode & S_IXGID)
    hdr->mode |= 02000;
#endif
#ifdef S_ISVTX
  if (sbuf.st_mode & S_ISVTX)
    hdr->mode |= 01000;
#endif

  hdr->nlink = sbuf.st_nlink;

  hdr->uid = sbuf.st_uid;
  hdr->gid = sbuf.st_gid;

  /* Only set the size for a regular file.  */
  if (hdr->typeflag == TF_REGULAR)
    hdr->size = sbuf.st_size;

  hdr->mtime = sbuf.st_mtime;
  

  return 0;
}



static gpg_error_t
add_entry (const char *dname, size_t dnamelen, struct dirent *de,
           scanctrl_t scanctrl)
{
  gpg_error_t err;
  tar_header_t hdr;
  char *p;

  assert (dnamelen);

  hdr = xtrycalloc (1, sizeof *hdr + dnamelen + 1
                    + (de? strlen (de->d_name) : 0));
  if (!hdr)
    {
      err = gpg_error_from_syserror ();
      log_error (_("error reading directory `%s': %s\n"),
                 dname, gpg_strerror (err));
      return err;
    }

  p = stpcpy (hdr->name, dname);
  if (de)
    {
      if (dname[dnamelen-1] != '/')
        *p++ = '/';
      strcpy (p, de->d_name);
    }
  else
    {
      if (hdr->name[dnamelen-1] == '/')
        hdr->name[dnamelen-1] = 0;
    }
#ifdef HAVE_DOSISH_SYSTEM
  for (p=hdr->name; *p; p++)
    if (*p == '\\')
      *p = '/';
#endif
  err = fillup_entry (hdr);
  if (err)
    xfree (hdr);
  else
    {
      if (opt.verbose)
        gpgtar_print_header (hdr, log_get_stream ());
      *scanctrl->flist_tail = hdr;
      scanctrl->flist_tail = &hdr->next;
    }

  return 0;
}


static gpg_error_t
scan_directory (const char *dname, scanctrl_t scanctrl)
{
  gpg_error_t err = 0;
  size_t dnamelen;
  DIR *dir;
  struct dirent *de;

  dnamelen = strlen (dname);
  if (!dnamelen)
    return 0;  /* An empty directory name has no entries.  */

  dir = opendir (dname);
  if (!dir)
    {
      err = gpg_error_from_syserror ();
      log_error (_("error reading directory `%s': %s\n"),
                 dname, gpg_strerror (err));
      return err;
    }
  
  while ((de = readdir (dir)))
    {
      if (!strcmp (de->d_name, "." ) || !strcmp (de->d_name, ".."))
        continue; /* Skip self and parent dir entry.  */
      
      err = add_entry (dname, dnamelen, de, scanctrl);
      if (err)
        goto leave;
     }

 leave:
  closedir (dir);
  return err;
}


static gpg_error_t
scan_recursive (const char *dname, scanctrl_t scanctrl)
{
  gpg_error_t err = 0;
  tar_header_t hdr, *start_tail, *stop_tail;

  if (scanctrl->nestlevel > 200)
    {
      log_error ("directories too deeply nested\n");
      return gpg_error (GPG_ERR_RESOURCE_LIMIT);
    }
  scanctrl->nestlevel++;

  assert (scanctrl->flist_tail);
  start_tail = scanctrl->flist_tail;
  scan_directory (dname, scanctrl);
  stop_tail = scanctrl->flist_tail;
  hdr = *start_tail;
  for (; hdr && hdr != *stop_tail; hdr = hdr->next)
    if (hdr->typeflag == TF_DIRECTORY)
      {
        if (opt.verbose > 1)
          log_info ("scanning directory `%s'\n", hdr->name);
        scan_recursive (hdr->name, scanctrl);
      }
  
  scanctrl->nestlevel--;
  return err;
}


/* Returns true if PATTERN is acceptable.  */
static int
pattern_valid_p (const char *pattern)
{
  if (!*pattern)
    return 0;
  if (*pattern == '.' && pattern[1] == '.')
    return 0;
  if (*pattern == '/' || *pattern == DIRSEP_C)
    return 0; /* Absolute filenames are not supported.  */
#ifdef HAVE_DRIVE_LETTERS
  if (((*pattern >= 'a' && *pattern <= 'z')
       || (*pattern >= 'A' && *pattern <= 'Z'))
      && pattern[1] == ':')
    return 0; /* Drive letter are not allowed either.  */
#endif /*HAVE_DRIVE_LETTERS*/ 

  return 1; /* Okay.  */
}



static void
store_xoctal (char *buffer, size_t length, unsigned long long value)
{
  char *p, *pend;
  size_t n;
  unsigned long long v;

  assert (length > 1);

  v = value;
  n = length;
  p = pend = buffer + length;
  *--p = 0; /* Nul byte.  */
  n--;
  do
    {
      *--p = '0' + (v % 8);
      v /= 8;
      n--;
    }
  while (v && n);
  if (!v)
    {
      /* Pad.  */
      for ( ; n; n--)
        *--p = '0';
    }
  else /* Does not fit into the field.  Store as binary number.  */
    {
      v = value;
      n = length;
      p = pend = buffer + length;
      do
        {
          *--p = v;
          v /= 256;
          n--;
        }
      while (v && n);
      if (!v)
        {
          /* Pad.  */
          for ( ; n; n--)
            *--p = 0;
          if (*p & 0x80)
            BUG ();
          *p |= 0x80; /* Set binary flag.  */
        }
      else
        BUG ();
    }
}


static void
store_uname (char *buffer, size_t length, unsigned long uid)
{
  static int initialized;
  static unsigned long lastuid;
  static char lastuname[32];

  if (!initialized || uid != lastuid)
    {
      struct passwd *pw = getpwuid (uid);

      lastuid = uid;
      initialized = 1;
      if (pw)
        mem2str (lastuname, pw->pw_name, sizeof lastuname); 
      else
        {
          log_info ("failed to get name for uid %lu\n", uid);
          *lastuname = 0;
        }
    }
  mem2str (buffer, lastuname, length);
}


static void
store_gname (char *buffer, size_t length, unsigned long gid)
{
  static int initialized;
  static unsigned long lastgid;
  static char lastgname[32];

  if (!initialized || gid != lastgid)
    {
      struct group *gr = getgrgid (gid);

      lastgid = gid;
      initialized = 1;
      if (gr)
        mem2str (lastgname, gr->gr_name, sizeof lastgname); 
      else
        {
          log_info ("failed to get name for gid %lu\n", gid);
          *lastgname = 0;
        }
    }
  mem2str (buffer, lastgname, length);
}


static gpg_error_t
build_header (void *record, tar_header_t hdr)
{
  gpg_error_t err;
  struct ustar_raw_header *raw = record;
  size_t namelen, n;
  unsigned long chksum;
  unsigned char *p;

  memset (record, 0, RECORDSIZE);

  /* Store name and prefix.  */
  namelen = strlen (hdr->name);
  if (namelen < sizeof raw->name)
    memcpy (raw->name, hdr->name, namelen);
  else 
    {
      n = (namelen < sizeof raw->prefix)? namelen : sizeof raw->prefix;
      for (n--; n ; n--)
        if (hdr->name[n] == '/')
          break;
      if (namelen - n < sizeof raw->name)
        {
          /* Note that the N is < sizeof prefix and that the
             delimiting slash is not stored.  */
          memcpy (raw->prefix, hdr->name, n);
          memcpy (raw->name, hdr->name+n+1, namelen - n);
        }
      else
        {
          err = gpg_error (GPG_ERR_TOO_LARGE);
          log_error ("error storing file `%s': %s\n", 
                     hdr->name, gpg_strerror (err));
          return err;
        }
    }
  
  store_xoctal (raw->mode,  sizeof raw->mode,  hdr->mode);
  store_xoctal (raw->uid,   sizeof raw->uid,   hdr->uid);
  store_xoctal (raw->gid,   sizeof raw->gid,   hdr->gid);
  store_xoctal (raw->size,  sizeof raw->size,  hdr->size);
  store_xoctal (raw->mtime, sizeof raw->mtime, hdr->mtime);

  switch (hdr->typeflag)
    {
    case TF_REGULAR:   raw->typeflag[0] = '0'; break;
    case TF_HARDLINK:  raw->typeflag[0] = '1'; break;
    case TF_SYMLINK:   raw->typeflag[0] = '2'; break;
    case TF_CHARDEV:   raw->typeflag[0] = '3'; break;
    case TF_BLOCKDEV:  raw->typeflag[0] = '4'; break;
    case TF_DIRECTORY: raw->typeflag[0] = '5'; break;
    case TF_FIFO:      raw->typeflag[0] = '6'; break;
    default: return gpg_error (GPG_ERR_NOT_SUPPORTED);
    }

  memcpy (raw->magic, "ustar", 6);
  raw->version[0] = '0';
  raw->version[1] = '0';

  store_uname (raw->uname, sizeof raw->uname, hdr->uid);
  store_gname (raw->gname, sizeof raw->gname, hdr->gid);

  if (hdr->typeflag == TF_SYMLINK)
    {
      int nread;

      nread = readlink (hdr->name, raw->linkname, sizeof raw->linkname -1);
      if (nread < 0)
        {
          err = gpg_error_from_syserror ();
          log_error ("error reading symlink `%s': %s\n", 
                     hdr->name, gpg_strerror (err));
          return err;
        }
      raw->linkname[nread] = 0;
    }
  

  /* Compute the checksum.  */
  memset (raw->checksum, ' ', sizeof raw->checksum);
  chksum = 0;
  p = record;
  for (n=0; n < RECORDSIZE; n++)
    chksum += *p++;
  store_xoctal (raw->checksum, sizeof raw->checksum - 1, chksum);
  raw->checksum[7] = ' ';

  return 0;
}


static gpg_error_t
write_file (estream_t stream, tar_header_t hdr)
{
  gpg_error_t err;
  char record[RECORDSIZE];
  estream_t infp;
  size_t nread, nbytes;
  int any;

  err = build_header (record, hdr);
  if (err)
    {
      if (gpg_err_code (err) == GPG_ERR_NOT_SUPPORTED)
        {
          log_info ("skipping unsupported file `%s'\n", hdr->name);
          err = 0;
        }
      return err;
    }

  if (hdr->typeflag == TF_REGULAR)
    {
      infp = es_fopen (hdr->name, "rb");
      if (!infp)
        {
          err = gpg_error_from_syserror ();
          log_error ("can't open `%s': %s - skipped\n",
                     hdr->name, gpg_strerror (err));
          return err;
        }
    }
  else
    infp = NULL;

  err = write_record (stream, record);
  if (err)
    goto leave;

  if (hdr->typeflag == TF_REGULAR)
    {
      hdr->nrecords = (hdr->size + RECORDSIZE-1)/RECORDSIZE;
      any = 0;
      while (hdr->nrecords--)
        {
          nbytes = hdr->nrecords? RECORDSIZE : (hdr->size % RECORDSIZE);
          nread = es_fread (record, 1, nbytes, infp);
          if (nread != nbytes)
            {
              err = gpg_error_from_syserror ();
              log_error ("error reading file `%s': %s%s\n",
                         hdr->name, gpg_strerror (err),
                         any? " (file shrunk?)":"");
              goto leave;
            }
          any = 1;
          err = write_record (stream, record);
          if (err)
            goto leave;
        }
      nread = es_fread (record, 1, 1, infp);
      if (nread)
        log_info ("note: file `%s' has grown\n", hdr->name);
    }

 leave:
  if (err)
    es_fclose (infp);
  else if ((err = es_fclose (infp)))
    log_error ("error closing file `%s': %s\n", hdr->name, gpg_strerror (err));
      
  return err;
}


static gpg_error_t
write_eof_mark (estream_t stream)
{
  gpg_error_t err;
  char record[RECORDSIZE];

  memset (record, 0, sizeof record);
  err = write_record (stream, record);
  if (!err)
    err = write_record (stream, record);
  return err;
}



void
gpgtar_create (char **inpattern)
{
  gpg_error_t err = 0;
  const char *pattern;
  struct scanctrl_s scanctrl_buffer;
  scanctrl_t scanctrl = &scanctrl_buffer;
  tar_header_t hdr, *start_tail;
  estream_t outstream;

  memset (scanctrl, 0, sizeof *scanctrl);
  scanctrl->flist_tail = &scanctrl->flist;

  for (; (pattern = *inpattern); inpattern++)
    {
      if (!*pattern)
        continue;
      if (opt.verbose > 1)
        log_info ("scanning `%s'\n", pattern);

      start_tail = scanctrl->flist_tail;
      if (!pattern_valid_p (pattern))
        log_error ("skipping invalid name `%s'\n", pattern);
      else if (!add_entry (pattern, strlen (pattern), NULL, scanctrl)
               && *start_tail && ((*start_tail)->typeflag & TF_DIRECTORY))
        scan_recursive (pattern, scanctrl);
    }

  if (opt.outfile)
    {
      outstream = es_fopen (opt.outfile, "wb");
      if (!outstream)
        {
          err = gpg_error_from_syserror ();
          log_error (_("can't create `%s': %s\n"),
                     opt.outfile, gpg_strerror (err));
          goto leave;
        }
    }
  else
    {
      outstream = es_stdout;
    }

  for (hdr = scanctrl->flist; hdr; hdr = hdr->next)
    {
      err = write_file (outstream, hdr);
      if (err)
        goto leave;
    }
  err = write_eof_mark (outstream);

 leave:
  if (!err)
    {
      if (outstream != es_stdout)
        err = es_fclose (outstream);
      else
        err = es_fflush (outstream);
      outstream = NULL;
    }
  if (err)
    {
      log_error ("creating tarball `%s' failed: %s\n",
                 es_fname_get (outstream), gpg_strerror (err));
      if (outstream && outstream != es_stdout)
        es_fclose (outstream);
      if (opt.outfile)
        gnupg_remove (opt.outfile);
    }
  scanctrl->flist_tail = NULL;
  while ( (hdr = scanctrl->flist) )
    {
      scanctrl->flist = hdr->next;
      xfree (hdr);
    }
}
