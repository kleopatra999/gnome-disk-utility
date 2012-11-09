/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2008-2012 Red Hat, Inc.
 *
 * Licensed under GPL version 2 or later.
 *
 * Author: David Zeuthen <zeuthen@gmail.com>
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixoutputstream.h>

#include <glib-unix.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include <canberra-gtk.h>

#include "gduapplication.h"
#include "gduwindow.h"
#include "gdurestorediskimagedialog.h"
#include "gduvolumegrid.h"
#include "gduestimator.h"

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  volatile gint ref_count;

  GduWindow *window;
  UDisksObject *object;
  UDisksBlock *block;
  UDisksDrive *drive;

  GtkBuilder *builder;
  GtkWidget *dialog;

  GtkWidget *infobar_vbox;
  GtkWidget *warning_infobar;
  GtkWidget *warning_label;
  GtkWidget *error_infobar;
  GtkWidget *error_label;

  GtkWidget *image_key_label;
  GtkWidget *image_fcbutton;

  GtkWidget *source_key_label;
  GtkWidget *source_label;
  GtkWidget *destination_key_label;
  GtkWidget *destination_label;

  GtkWidget *copying_key_label;
  GtkWidget *copying_vbox;
  GtkWidget *copying_progressbar;
  GtkWidget *copying_label;

  GtkWidget *start_copying_button;
  GtkWidget *close_button;
  GtkWidget *cancel_button;

  guint64 block_size;
  gint64 start_time_usec;
  gint64 end_time_usec;

  GCancellable *cancellable;
  GOutputStream *block_stream;
  GFileInputStream *file_input_stream;
  guint64 file_size;

  guchar *buffer;
  guint64 total_bytes_read;
  guint64 buffer_bytes_written;
  guint64 buffer_bytes_to_write;

  /* must hold copy_lock when reading/writing these */
  GMutex copy_lock;
  GduEstimator *estimator;
  guint update_id;
  GError *copy_error;

  guint inhibit_cookie;

  gboolean completed;
} DialogData;


static const struct {
  goffset offset;
  const gchar *name;
} widget_mapping[] = {
  {G_STRUCT_OFFSET (DialogData, infobar_vbox), "infobar-vbox"},

  {G_STRUCT_OFFSET (DialogData, image_key_label), "image-key-label"},
  {G_STRUCT_OFFSET (DialogData, image_fcbutton), "image-fcbutton"},
  {G_STRUCT_OFFSET (DialogData, source_key_label), "source-key-label"},
  {G_STRUCT_OFFSET (DialogData, source_label), "source-label"},
  {G_STRUCT_OFFSET (DialogData, destination_key_label), "destination-key-label"},
  {G_STRUCT_OFFSET (DialogData, destination_label), "destination-label"},

  {G_STRUCT_OFFSET (DialogData, copying_key_label), "copying-key-label"},
  {G_STRUCT_OFFSET (DialogData, copying_vbox), "copying-vbox"},
  {G_STRUCT_OFFSET (DialogData, copying_progressbar), "copying-progressbar"},
  {G_STRUCT_OFFSET (DialogData, copying_label), "copying-label"},

  {G_STRUCT_OFFSET (DialogData, start_copying_button), "start-copying-button"},
  {G_STRUCT_OFFSET (DialogData, cancel_button), "cancel-button"},
  {G_STRUCT_OFFSET (DialogData, close_button), "close-button"},
  {0, NULL}
};

/* ---------------------------------------------------------------------------------------------------- */

static DialogData *
dialog_data_ref (DialogData *data)
{
  g_atomic_int_inc (&data->ref_count);
  return data;
}

static void
dialog_data_unref (DialogData *data)
{
  if (g_atomic_int_dec_and_test (&data->ref_count))
    {
      /* hide the dialog */
      if (data->dialog != NULL)
        {
          GtkWidget *dialog;
          dialog = data->dialog;
          data->dialog = NULL;
          gtk_widget_hide (dialog);
          gtk_widget_destroy (dialog);
        }
      g_object_unref (data->warning_infobar);
      g_object_unref (data->error_infobar);

      g_object_unref (data->window);
      g_object_unref (data->object);
      g_object_unref (data->block);
      g_clear_object (&data->drive);
      if (data->builder != NULL)
        g_object_unref (data->builder);
      g_free (data->buffer);
      g_clear_object (&data->estimator);

      g_clear_object (&data->cancellable);
      g_clear_object (&data->file_input_stream);
      g_clear_object (&data->block_stream);
      g_mutex_clear (&data->copy_lock);
      g_free (data);
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
unref_in_idle (gpointer user_data)
{
  DialogData *data = user_data;
  dialog_data_unref (data);
  return FALSE; /* remove source */
}

static void
dialog_data_unref_in_idle (DialogData *data)
{
  g_idle_add (unref_in_idle, data);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
dialog_data_complete_and_unref (DialogData *data)
{
  if (!data->completed)
    {
      data->completed = TRUE;
      g_cancellable_cancel (data->cancellable);
    }
  if (data->inhibit_cookie > 0)
    {
      gtk_application_uninhibit (GTK_APPLICATION (gdu_window_get_application (data->window)),
                                 data->inhibit_cookie);
      data->inhibit_cookie = 0;
    }
  gtk_widget_hide (data->dialog);
  dialog_data_unref (data);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
restore_disk_image_update (DialogData *data)
{
  gboolean can_proceed = FALSE;
  gchar *restore_warning = NULL;
  gchar *restore_error = NULL;
  GFile *restore_file = NULL;

  if (data->dialog == NULL)
    goto out;

  /* don't update if we're already copying */
  if (data->buffer != NULL)
    goto out;

  /* Check if we have a file */
  restore_file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (data->image_fcbutton));
  if (restore_file != NULL)
    {
      GFileInfo *info;
      guint64 size;
      gchar *s;
      info = g_file_query_info (restore_file,
                                G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                G_FILE_QUERY_INFO_NONE,
                                NULL,
                                NULL);
      size = g_file_info_get_size (info);
      g_object_unref (info);

      if (data->block_size > 0)
        {
          if (size == 0)
            {
              restore_error = g_strdup (_("Cannot restore image of size 0"));
            }
          else if (size < data->block_size)
            {
              /* Only complain if slack is bigger than 1MB */
              if (data->block_size - size > 1000L*1000L)
                {
                  s = udisks_client_get_size_for_display (gdu_window_get_client (data->window),
                                                          data->block_size - size, FALSE, FALSE);
                  restore_warning = g_strdup_printf (_("The disk image is %s smaller than the target device"), s);
                  g_free (s);
                }
              can_proceed = TRUE;
            }
          else if (size > data->block_size)
            {
              s = udisks_client_get_size_for_display (gdu_window_get_client (data->window),
                                                      size - data->block_size, FALSE, FALSE);
              restore_error = g_strdup_printf (_("The disk image is %s bigger than the target device"), s);
              g_free (s);
            }
          else
            {
              /* all good */
              can_proceed = TRUE;
            }
        }
    }

  if (restore_warning != NULL)
    {
      gtk_label_set_text (GTK_LABEL (data->warning_label), restore_warning);
      gtk_widget_show (data->warning_infobar);
    }
  else
    {
      gtk_widget_hide (data->warning_infobar);
    }
  if (restore_error != NULL)
    {
      gtk_label_set_text (GTK_LABEL (data->error_label), restore_error);
      gtk_widget_show (data->error_infobar);
    }
  else
    {
      gtk_widget_hide (data->error_infobar);
    }

  g_free (restore_warning);
  g_free (restore_error);
  g_clear_object (&restore_file);

  gtk_dialog_set_response_sensitive (GTK_DIALOG (data->dialog), GTK_RESPONSE_OK, can_proceed);

 out:
  ;
}

static void
on_file_set (GtkFileChooserButton   *button,
             gpointer                user_data)
{
  DialogData *data = user_data;
  if (data->dialog == NULL)
    goto out;
  restore_disk_image_update (data);
 out:
  ;
}

static void
on_notify (GObject    *object,
           GParamSpec *pspec,
           gpointer    user_data)
{
  DialogData *data = user_data;
  if (data->dialog == NULL)
    goto out;
  restore_disk_image_update (data);
 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
restore_disk_image_populate (DialogData *data)
{
  UDisksObjectInfo *info;

  gdu_utils_configure_file_chooser_for_disk_images (GTK_FILE_CHOOSER (data->image_fcbutton), TRUE);

  /* Destination label */
  info = udisks_client_get_object_info (gdu_window_get_client (data->window), data->object);
  gtk_label_set_text (GTK_LABEL (data->destination_label), info->one_liner);
  udisks_object_info_unref (info);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_gui (DialogData *data,
            gboolean    done)
{
  guint64 bytes_completed = 0;
  guint64 bytes_target = 1;
  guint64 bytes_per_sec = 0;
  guint64 usec_remaining = 1;
  gchar *s, *s2, *s3, *s4, *s5;
  gdouble progress;

  g_mutex_lock (&data->copy_lock);
  if (data->estimator != NULL)
    {
      bytes_per_sec = gdu_estimator_get_bytes_per_sec (data->estimator);
      usec_remaining = gdu_estimator_get_usec_remaining (data->estimator);
      bytes_completed = gdu_estimator_get_completed_bytes (data->estimator);
      bytes_target = gdu_estimator_get_target_bytes (data->estimator);
    }
  data->update_id = 0;
  g_mutex_unlock (&data->copy_lock);

  if (done)
    {
      gint64 duration_usec = data->end_time_usec - data->start_time_usec;
      s2 = g_format_size (bytes_target);
      s3 = gdu_utils_format_duration_usec (duration_usec, GDU_FORMAT_DURATION_FLAGS_SUBSECOND_PRECISION);
      s4 = g_format_size (G_USEC_PER_SEC * bytes_target / duration_usec);
      /* Translators: string used for conveying how long the copy took.
       *              The first %s is the amount of bytes copied (ex. "650 MB").
       *              The second %s is the time it took to copy (ex. "1 minute", or "Less than a minute").
       *              The third %s is the average amount of bytes transfered per second (ex. "8.9 MB").
       */
      s = g_strdup_printf (_("%s copied in %s (%s/sec)"), s2, s3, s4);
      g_free (s4);
      g_free (s3);
      g_free (s2);
    }
  else if (bytes_per_sec > 0 && usec_remaining > 0)
    {
      s2 = g_format_size (bytes_completed);
      s3 = g_format_size (bytes_target);
      s4 = gdu_utils_format_duration_usec (usec_remaining,
                                           GDU_FORMAT_DURATION_FLAGS_NO_SECONDS);
      s5 = g_format_size (bytes_per_sec);
      /* Translators: string used for conveying progress of copy operation when there are no errors.
       *              The first %s is the amount of bytes copied (ex. "650 MB").
       *              The second %s is the size of the device (ex. "8.5 GB").
       *              The third %s is the estimated amount of time remaining (ex. "1 minute" or "5 minutes").
       *              The fourth %s is the average amount of bytes transfered per second (ex. "8.9 MB").
       */
      s = g_strdup_printf (_("%s of %s copied – %s remaining (%s/sec)"), s2, s3, s4, s5);
      g_free (s5);
      g_free (s4);
      g_free (s3);
      g_free (s2);
    }
  else
    {
      s2 = g_format_size (bytes_completed);
      s3 = g_format_size (bytes_target);
      /* Translators: string used for convey progress of a copy operation where we don't know time remaining / speed.
       * The first two %s are strings with the amount of bytes (ex. "3.4 MB" and "300 MB").
       */
      s = g_strdup_printf (_("%s of %s copied"), s2, s3);
      g_free (s2);
      g_free (s3);
    }

  s2 = g_strconcat ("<small>", s, "</small>", NULL);
  gtk_label_set_markup (GTK_LABEL (data->copying_label), s2);
  g_free (s);

  if (done)
    progress = 1.0;
  else
    progress = ((gdouble) bytes_completed) / ((gdouble) bytes_target);
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (data->copying_progressbar), progress);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
play_complete_sound_and_uninhibit (DialogData *data)
{
  const gchar *sound_message;

  /* Translators: A descriptive string for the 'complete' sound, see CA_PROP_EVENT_DESCRIPTION */
  sound_message = _("Disk image copying complete");
  ca_gtk_play_for_widget (GTK_WIDGET (data->dialog), 0,
                          CA_PROP_EVENT_ID, "complete",
                          CA_PROP_EVENT_DESCRIPTION, sound_message,
                          NULL);

  if (data->inhibit_cookie > 0)
    {
      gtk_application_uninhibit (GTK_APPLICATION (gdu_window_get_application (data->window)),
                                 data->inhibit_cookie);
      data->inhibit_cookie = 0;
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
on_update_ui (gpointer user_data)
{
  DialogData *data = user_data;
  update_gui (data, FALSE);
  dialog_data_unref (data);
  return FALSE; /* remove source */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
on_show_error (gpointer user_data)
{
  DialogData *data = user_data;

  play_complete_sound_and_uninhibit (data);

  g_assert (data->copy_error != NULL);
  gdu_utils_show_error (GTK_WINDOW (data->dialog),
                        _("Error restoring disk image"),
                        data->copy_error);
  g_clear_error (&data->copy_error);

  dialog_data_complete_and_unref (data);

  dialog_data_unref (data);
  return FALSE; /* remove source */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
on_success (gpointer user_data)
{
  DialogData *data = user_data;

  update_gui (data, TRUE);

  gtk_widget_hide (data->cancel_button);
  gtk_widget_show (data->close_button);

  play_complete_sound_and_uninhibit (data);

  dialog_data_unref (data);
  return FALSE; /* remove source */
}

/* ---------------------------------------------------------------------------------------------------- */

static gpointer
copy_thread_func (gpointer user_data)
{
  DialogData *data = user_data;
  guchar *buffer_unaligned = NULL;
  guchar *buffer = NULL;
  guint64 block_device_size = 0;
  long page_size;
  GError *error = NULL;
  GError *error2 = NULL;
  gint64 last_update_usec = -1;
  gint fd = -1;
  gint buffer_size;
  guint64 num_bytes_completed = 0;

  /* default to 1 MiB blocks */
  buffer_size = (1 * 1024 * 1024);

  /* Most OSes put ACLs for logged-in users on /dev/sr* nodes (this is
   * so CD burning tools etc. work) so see if we can open the device
   * file ourselves. If so, great, since this avoids a polkit dialog.
   *
   * As opposed to udisks' OpenForBackup() we also avoid O_EXCL since
   * the disc is read-only by its very nature. As a side-effect this
   * allows creating a disk image of a mounted disc.
   */
  if (g_str_has_prefix (udisks_block_get_device (data->block), "/dev/sr"))
    {
      fd = open (udisks_block_get_device (data->block), O_RDONLY);
    }

  /* Otherwise, request the fd from udisks */
  if (fd == -1)
    {
      GUnixFDList *fd_list = NULL;
      GVariant *fd_index = NULL;
      if (!udisks_block_call_open_for_restore_sync (data->block,
                                                    g_variant_new ("a{sv}", NULL), /* options */
                                                    NULL, /* fd_list */
                                                    &fd_index,
                                                    &fd_list,
                                                    NULL, /* cancellable */
                                                    &error))
        goto out;

      fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_index), &error);
      if (error != NULL)
        {
          g_prefix_error (&error,
                          "Error extracing fd with handle %d from D-Bus message: ",
                          g_variant_get_handle (fd_index));
          goto out;
        }
      if (fd_index != NULL)
        g_variant_unref (fd_index);
      g_clear_object (&fd_list);
    }

  g_assert (fd != -1);

  /* We can't use udisks_block_get_size() because the media may have
   * changed and udisks may not have noticed. TODO: maybe have a
   * Block.GetSize() method instead...
   */
  if (ioctl (fd, BLKGETSIZE64, &block_device_size) != 0)
    {
      error = g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                           "%s", strerror (errno));
      g_prefix_error (&error, _("Error determining size of device: "));
      goto out;
    }

  if (block_device_size == 0)
    {
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Device is size 0"));
      goto out;
    }
  data->block_size = block_device_size;

  page_size = sysconf (_SC_PAGESIZE);
  buffer_unaligned = g_new0 (guchar, buffer_size + page_size);
  buffer = (guchar*) (((gintptr) (buffer_unaligned + page_size)) & (~(page_size - 1)));

  g_mutex_lock (&data->copy_lock);
  data->estimator = gdu_estimator_new (data->file_size);
  data->update_id = 0;
  data->start_time_usec = g_get_real_time ();
  g_mutex_unlock (&data->copy_lock);

  /* Read huge (e.g. 1 MiB) blocks and write it to the output
   * device even if it was only partially read.
   */
  num_bytes_completed = 0;
  while (num_bytes_completed < data->file_size)
    {
      gsize num_bytes_to_read;
      gsize num_bytes_read;
      ssize_t num_bytes_written;
      gint64 now_usec;

      num_bytes_to_read = buffer_size;
      if (num_bytes_to_read + num_bytes_completed > data->file_size)
        num_bytes_to_read = data->file_size - num_bytes_completed;

      /* Update GUI - but only every 200 ms and only if last update isn't pending */
      g_mutex_lock (&data->copy_lock);
      now_usec = g_get_monotonic_time ();
      if (now_usec - last_update_usec > 200 * G_USEC_PER_SEC / 1000 || last_update_usec < 0)
        {
          if (num_bytes_completed > 0)
            gdu_estimator_add_sample (data->estimator, num_bytes_completed);
          if (data->update_id == 0)
            data->update_id = g_idle_add (on_update_ui, dialog_data_ref (data));
          last_update_usec = now_usec;
        }
      g_mutex_unlock (&data->copy_lock);

      if (!g_input_stream_read_all (G_INPUT_STREAM (data->file_input_stream),
                                    buffer,
                                    num_bytes_to_read,
                                    &num_bytes_read,
                                    data->cancellable,
                                    &error))
        {
          g_prefix_error (&error,
                          "Error reading %" G_GUINT64_FORMAT " bytes from offset %" G_GUINT64_FORMAT ": ",
                          num_bytes_to_read,
                          num_bytes_completed);
          goto out;
        }
      if (num_bytes_read != num_bytes_to_read)
        {
          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Requested %" G_GUINT64_FORMAT " bytes from offset %" G_GUINT64_FORMAT " but only read %" G_GUINT64_FORMAT " bytes",
                       num_bytes_read,
                       num_bytes_completed,
                       num_bytes_to_read);
          goto out;
        }

    copy_write_again:
      num_bytes_written = write (fd, buffer, num_bytes_read);
      if (num_bytes_written < 0)
        {
          if (errno == EAGAIN || errno == EINTR)
            goto copy_write_again;

          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Error writing %" G_GUINT64_FORMAT " bytes to offset %" G_GUINT64_FORMAT ": %m",
                       num_bytes_read,
                       num_bytes_completed);
          goto out;
        }

      /*g_print ("copied %" G_GUINT64_FORMAT " bytes at offset %" G_GUINT64_FORMAT "\n",
               (guint64) num_bytes_written,
               num_bytes_completed);*/

      num_bytes_completed += num_bytes_written;
    }

 out:
  data->end_time_usec = g_get_real_time ();

  /* in either case, close the stream */
  if (!g_input_stream_close (G_INPUT_STREAM (data->file_input_stream),
                              NULL, /* cancellable */
                              &error2))
    {
      g_warning ("Error closing file input stream: %s (%s, %d)",
                 error2->message, g_quark_to_string (error2->domain), error2->code);
      g_clear_error (&error2);
    }
  g_clear_object (&data->file_input_stream);

  if (fd != -1 )
    {
      if (close (fd) != 0)
        g_warning ("Error closing fd: %m");
    }

  if (error != NULL)
    {
      /* show error in GUI */
      if (!(error->domain == G_IO_ERROR && error->code == G_IO_ERROR_CANCELLED))
        {
          data->copy_error = error; error = NULL;
          g_idle_add (on_show_error, dialog_data_ref (data));
        }
      g_clear_error (&error);

      /* Wipe the device */
      if (!udisks_block_call_format_sync (data->block,
                                          "empty",
                                          g_variant_new ("a{sv}", NULL), /* options */
                                          NULL, /* cancellable */
                                          &error2))
        {
          g_warning ("Error wiping device on error path: %s (%s, %d)",
                     error2->message, g_quark_to_string (error2->domain), error2->code);
          g_clear_error (&error2);
        }
    }
  else
    {
      /* success */
      g_idle_add (on_success, dialog_data_ref (data));
    }

  g_free (buffer_unaligned);

  /* finally, request that the core OS / kernel rescans the device */
  if (!udisks_block_call_rescan_sync (data->block,
                                      g_variant_new ("a{sv}", NULL), /* options */
                                      NULL, /* cancellable */
                                      &error2))
    {
      g_warning ("Error rescanning device: %s (%s, %d)",
                 error2->message, g_quark_to_string (error2->domain), error2->code);
      g_clear_error (&error2);
    }

  dialog_data_unref_in_idle (data); /* unref on main thread */
  return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
start_copying (DialogData *data)
{
  GFile *file = NULL;
  gboolean ret = FALSE;
  GFileInfo *info;
  GError *error;
  gchar *uri = NULL;

  error = NULL;
  file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (data->image_fcbutton));
  data->file_input_stream = g_file_read (file,
                                         NULL,
                                         &error);
  if (data->file_input_stream == NULL)
    {
      if (!(error->domain == G_IO_ERROR && error->code == G_IO_ERROR_CANCELLED))
        gdu_utils_show_error (GTK_WINDOW (data->dialog), _("Error opening file for reading"), error);
      g_error_free (error);
      dialog_data_complete_and_unref (data);
      goto out;
    }

  error = NULL;
  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            &error);
  if (info == NULL)
    {
      gdu_utils_show_error (GTK_WINDOW (data->dialog), _("Error determing size of file"), error);
      g_error_free (error);
      dialog_data_complete_and_unref (data);
      goto out;
    }
  data->file_size = g_file_info_get_size (info);
  g_object_unref (info);

  uri = gdu_utils_get_pretty_uri (file);
  gtk_label_set_text (GTK_LABEL (data->source_label), uri);

  data->inhibit_cookie = gtk_application_inhibit (GTK_APPLICATION (gdu_window_get_application (data->window)),
                                                  GTK_WINDOW (data->dialog),
                                                  GTK_APPLICATION_INHIBIT_SUSPEND |
                                                  GTK_APPLICATION_INHIBIT_LOGOUT,
                                                  /* Translators: Reason why suspend/logout is being inhibited */
                                                  C_("restore-inhibit-message", "Copying disk image to device"));

  g_thread_new ("copy-disk-image-thread",
                copy_thread_func,
                dialog_data_ref (data));
  ret = TRUE;

 out:
  g_clear_object (&file);
  g_free (uri);
  return ret;
}

static void
on_dialog_response (GtkDialog     *dialog,
                    gint           response,
                    gpointer       user_data)
{
  DialogData *data = user_data;
  GList *objects = NULL;

  if (data->dialog == NULL)
    goto out;

  objects = g_list_append (NULL, data->object);

  switch (response)
    {
    case GTK_RESPONSE_OK:
      if (!gdu_utils_show_confirmation (GTK_WINDOW (data->dialog),
                                        _("Are you sure you want to write the disk image to the device?"),
                                        _("All existing data will be lost"),
                                        _("_Restore"),
                                        NULL, NULL,
                                        gdu_window_get_client (data->window), objects))
        {
          dialog_data_complete_and_unref (data);
          goto out;
        }

      /* now that we know the user picked a folder, update file chooser settings */
      gdu_utils_file_chooser_for_disk_images_update_settings (GTK_FILE_CHOOSER (data->image_fcbutton));

      /* now that we advance to the "copy stage", hide/show some more widgets */
      gtk_widget_hide (data->image_key_label);
      gtk_widget_hide (data->image_fcbutton);
      gtk_widget_hide (data->start_copying_button);
      gtk_widget_show (data->source_key_label);
      gtk_widget_show (data->source_label);
      gtk_widget_show (data->copying_key_label);
      gtk_widget_show (data->copying_vbox);
      gtk_widget_hide (data->infobar_vbox);

      start_copying (data);
      break;

    case GTK_RESPONSE_CLOSE:
      dialog_data_complete_and_unref (data);
      break;

    default: /* explicit fallthrough */
    case GTK_RESPONSE_CANCEL:
      dialog_data_complete_and_unref (data);
      break;
    }
 out:
  g_list_free (objects);
}

void
gdu_restore_disk_image_dialog_show (GduWindow    *window,
                                    UDisksObject *object)
{
  guint n;
  DialogData *data;

  data = g_new0 (DialogData, 1);
  data->ref_count = 1;
  g_mutex_init (&data->copy_lock);
  data->window = g_object_ref (window);
  data->object = g_object_ref (object);
  data->block = udisks_object_get_block (object);
  g_assert (data->block != NULL);
  data->drive = udisks_client_get_drive_for_block (gdu_window_get_client (window), data->block);
  data->cancellable = g_cancellable_new ();

  /* TODO: use a method call for this so it works on e.g. floppy drives where e.g. we don't know the size */
  data->block_size = udisks_block_get_size (data->block);

  data->dialog = GTK_WIDGET (gdu_application_new_widget (gdu_window_get_application (data->window),
                                                         "restore-disk-image-dialog.ui",
                                                         "restore-disk-image-dialog",
                                                         &data->builder));
  for (n = 0; widget_mapping[n].name != NULL; n++)
    {
      gpointer *p = (gpointer *) ((char *) data + widget_mapping[n].offset);
      *p = gtk_builder_get_object (data->builder, widget_mapping[n].name);
    }
  g_signal_connect (data->image_fcbutton, "file-set", G_CALLBACK (on_file_set), data);

  data->warning_infobar = gdu_utils_create_info_bar (GTK_MESSAGE_INFO, "", &data->warning_label);
  gtk_box_pack_start (GTK_BOX (data->infobar_vbox), data->warning_infobar, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all (data->warning_infobar, TRUE);
  g_object_ref (data->warning_infobar);

  data->error_infobar = gdu_utils_create_info_bar (GTK_MESSAGE_ERROR, "", &data->error_label);
  gtk_box_pack_start (GTK_BOX (data->infobar_vbox), data->error_infobar, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all (data->error_infobar, TRUE);
  g_object_ref (data->error_infobar);

  restore_disk_image_populate (data);
  restore_disk_image_update (data);

  /* unfortunately, GtkFileChooserButton:file-set is not emitted when the user
   * unselects a file but we can work around that.. (TODO: file bug against gtk+)
   */
  g_signal_connect (data->image_fcbutton, "notify",
                    G_CALLBACK (on_notify), data);

  /* hide widgets not to be shown initially */
  gtk_widget_hide (data->source_key_label);
  gtk_widget_hide (data->source_label);
  gtk_widget_hide (data->copying_key_label);
  gtk_widget_hide (data->copying_vbox);
  gtk_widget_hide (data->close_button);

  g_signal_connect (data->dialog,
                    "response",
                    G_CALLBACK (on_dialog_response),
                    data);
  gtk_widget_show (data->dialog);
  gtk_window_present (GTK_WINDOW (data->dialog));
}

/* ---------------------------------------------------------------------------------------------------- */
