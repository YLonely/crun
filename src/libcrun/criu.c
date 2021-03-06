/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2020 Adrian Reber <areber@redhat.com>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE

#include <config.h>

#ifdef HAVE_CRIU

#include <unistd.h>
#include <sys/types.h>
#include <criu/criu.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "container.h"
#include "status.h"
#include "utils.h"

#define CRIU_CHECKPOINT_LOG_FILE "dump.log"

int
libcrun_container_checkpoint_linux_criu (libcrun_container_status_t *status,
                                         libcrun_container_t *container,
                                         libcrun_checkpoint_restore_t *
                                         cr_options, libcrun_error_t *err)
{
  runtime_spec_schema_config_schema *def = container->container_def;
  cleanup_free char *path = NULL;
  cleanup_close int image_fd = -1;
  cleanup_close int work_fd = -1;
  size_t i;
  int ret;

  if (geteuid ())
    return crun_make_error (err, 0, "Checkpointing requires root");

  /* No CRIU version or feature checking yet. In configure.ac there
   * is a minimum CRIU version listed and so far it is good enough.
   *
   * The CRIU library also does not yet have an interface to CRIU
   * the version of the binary. Right now it is only possible to
   * query the version of the library via defines during buildtime.
   *
   * The whole CRIU library setup works this way, that the library
   * is only a wrapper around RPC calls to the actual library. So
   * if CRIU is updated and the SO of the library does not change,
   * and crun is not rebuilt against the newer version, the version
   * is still returning the values during buildtime and not from
   * the actual running CRIU binary. The RPC interface between the
   * library will not break, so no reason to worry, but it is not
   * possible to detect (via the library) which CRIU version is
   * actually being used. This needs to be added to CRIU upstream. */

  ret = criu_init_opts ();
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, 0, "CRIU init failed with %d\n", ret);

  if (UNLIKELY (cr_options->image_path == NULL))
    return crun_make_error (err, 0, "image path not set\n");

  ret = mkdir (cr_options->image_path, 0700);
  if (UNLIKELY ((ret == -1) && (errno != EEXIST)))
    return crun_make_error (err, 0,
                            "error creating checkpoint directory %s\n",
                            cr_options->image_path);

  image_fd = open (cr_options->image_path, O_DIRECTORY);
  if (UNLIKELY (image_fd == -1))
    return crun_make_error (err, 0, "error opening checkpoint directory %s\n",
                            cr_options->image_path);

  criu_set_images_dir_fd (image_fd);

  /* work_dir is the place CRIU will put its logfiles. If not explicitly set,
   * CRIU will put the logfiles into the images_dir from above. No need for
   * crun to set it if the user has not selected a specific directory. */
  if (cr_options->work_path != NULL)
    {
      work_fd = open (cr_options->work_path, O_DIRECTORY);
      if (UNLIKELY (work_fd == -1))
        return crun_make_error (err, 0,
                                "error opening CRIU work directory %s\n",
                                cr_options->work_path);

      criu_set_work_dir_fd (work_fd);
    }
  else
    {
      /* This is only for the error message later. */
      cr_options->work_path = cr_options->image_path;
    }

  /* The main process of the container is the process CRIU will checkpoint
   * and all of its children. */
  criu_set_pid (status->pid);

  ret = xasprintf (&path, "%s/%s", status->bundle, status->rootfs);
  if (UNLIKELY (ret == -1))
    libcrun_fail_with_error (0, "xasprintf failed");

  ret = criu_set_root (path);
  if (UNLIKELY (ret != 0))
    return crun_make_error (err, 0, "error setting CRIU root to %s\n", path);

  /* Tell CRIU about external bind mounts. */
  for (i = 0; i < def->mounts_len; i++)
    {
      size_t j;

      for (j = 0; j < def->mounts[i]->options_len; j++)
        {
          if (strcmp (def->mounts[i]->options[j], "bind") == 0
              || strcmp (def->mounts[i]->options[j], "rbind") == 0)
            {
              criu_add_ext_mount (def->mounts[i]->destination,
                                  def->mounts[i]->destination);
              break;
            }
        }
    }

  /* Set boolean options . */
  criu_set_leave_running (cr_options->leave_running);
  criu_set_ext_unix_sk (cr_options->ext_unix_sk);
  criu_set_shell_job (cr_options->shell_job);
  criu_set_tcp_established (cr_options->tcp_established);

  /* Set up logging. */
  criu_set_log_level (4);
  criu_set_log_file (CRIU_CHECKPOINT_LOG_FILE);
  ret = criu_dump ();
  if (UNLIKELY (ret != 0))
    return crun_make_error (err, 0,
                            "CRIU checkpointing failed %d\n"
                            "Please check CRIU logfile %s/%s\n", ret,
                            cr_options->work_path, CRIU_CHECKPOINT_LOG_FILE);

  return 0;
}

#endif
