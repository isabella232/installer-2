// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos_postinst.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <vboot/CgptManager.h>

#include "chromeos_setimage.h"
#include "inst_util.h"

using std::string;

// Updates firmware. We must activate new firmware only after new kernel is
// actived (installed and made bootable), otherwise new firmware with all old
// kernels may lead to recovery screen (due to new key).
// TODO(hungte) Replace the shell execution by native code (crosbug.com/25407).
bool FirmwareUpdate(const string &install_dir, bool is_update) {
  int result;
  const char *mode;
  string command = install_dir + "/usr/sbin/chromeos-firmwareupdate";

  if (access(command.c_str(), X_OK) != 0) {
    printf("No firmware updates available.\n");
    return true;
  }

  // Binary compatibility test.
  string test_sh_command = install_dir + "/bin/sh -c exit";
  if (system(test_sh_command.c_str()) != 0) {
    printf("Detected incompatible system binary. "
           "Firmware updates are disabled for system architecture transition "
           "(ex, 32->64 bits) auto updates.\n");
    return true;
  }

  if (is_update) {
    // Background auto update by Update Engine.
    mode = "autoupdate";
  } else {
    // Recovery image, or from command "chromeos-install".
    mode = "recovery";
  }

  command += " --mode=";
  command += mode;

  printf("Starting firmware updater (%s)\n", command.c_str());
  result = system(command.c_str());

  // Next step after postinst may take a lot of time (eg, disk wiping)
  // and people may confuse that as 'firmware update takes a long wait',
  // we explicitly prompt here.
  if (result == 0) {
    printf("Firmware update completed.\n");
  } else if (result == 3) {
    printf("Firmware can't be updated because booted from B (error code: %d)\n",
           result);
  } else {
    printf("Firmware update failed (error code: %d).\n", result);
  }

  return result == 0;
}

// Matches commandline arguments of chrome-chroot-postinst
//
// src_version of the form "10.2.3.4" or "12.3.2"
// install_dev of the form "/dev/sda3"
//
bool ChromeosChrootPostinst(string install_dir,
                            string src_version,
                            bool firmware_update,
                            string install_dev) {

  printf("ChromeosChrootPostinst(%s, %s, %d, %s)\n",
         install_dir.c_str(), src_version.c_str(),
         firmware_update, install_dev.c_str());

  // Extract External ENVs
  bool is_factory_install = getenv("IS_FACTORY_INSTALL");
  bool is_recovery_install = getenv("IS_RECOVERY_INSTALL");
  bool is_install = getenv("IS_INSTALL");
  bool is_update = !is_factory_install && !is_recovery_install && !is_install;

  // Find Misc partition/device names
  string root_dev = GetBlockDevFromPartitionDev(install_dev);
  int new_part_num = GetPartitionFromPartitionDev(install_dev);
  int new_kern_num = new_part_num - 1;
  string new_k_dev = MakePartitionDev(root_dev, new_kern_num);

  string boot_slot;
  switch (new_part_num) {
    case 3:
      boot_slot = "A";
      break;
    case 5:
      boot_slot = "B";
      break;
    default:
      fprintf(stderr,
              "Not a valid target parition number: %i\n", new_part_num);
      return 1;
  }

  bool make_dev_readonly = false;

  if (is_update && VersionLess(src_version, "0.10.156.2")) {
    // See bug chromium-os:11517. This fixes an old FS corruption problem.
    printf("Patching new rootfs\n");
    if (!R10FileSystemPatch(install_dev))
      return false;
    make_dev_readonly=true;
  }

  // If this FS was mounted read-write, we can't do deltas from it. Mark the
  // FS as such
  Touch(install_dir + "/.nodelta");  // Ignore Error on purpse

  printf("Set boot target to %s: Partition %d, Slot %s\n",
         install_dev.c_str(),
         new_part_num,
         boot_slot.c_str());

  if (!SetImage(install_dir, root_dev, install_dev, new_k_dev)) {
    printf("SetImage failed.\n");
    return false;
  }

  printf("Syncing filesystems before changing boot order...\n");
  sync();

  printf("Updating Partition Table Attributes using CgptManager...\n");

  CgptManager cgpt_manager;

  int result = cgpt_manager.Initialize(root_dev);
  if (result != kCgptSuccess) {
    printf("Unable to initialize CgptManager\n");
    return false;
  }

  result = cgpt_manager.SetHighestPriority(new_kern_num);
  if (result != kCgptSuccess) {
    printf("Unable to set highest priority for kernel %d\n", new_kern_num);
    return false;
  }

  // If it's not an update, pre-mark the first boot as successful
  // since we can't fall back on the old install.
  bool new_kern_successful = !is_update;
  result = cgpt_manager.SetSuccessful(new_kern_num, new_kern_successful);
  if (result != kCgptSuccess) {
    printf("Unable to set successful to %d for kernel %d\n",
           new_kern_successful,
           new_kern_num);
    return false;
  }

  int numTries = 6;
  result = cgpt_manager.SetNumTriesLeft(new_kern_num, numTries);
  if (result != kCgptSuccess) {
    printf("Unable to set NumTriesLeft to %d for kernel %d\n",
           numTries,
           new_kern_num);
    return false;
  }

  printf("Updated kernel %d with Successful = %d and NumTriesLeft = %d\n",
          new_kern_num, new_kern_successful, numTries);

  if (make_dev_readonly) {
    printf("Making dev %s read-only\n", install_dev.c_str());
    MakeDeviceReadOnly(install_dev);  // Ignore error
  }

  // At this point in the script, the new partition has been marked bootable
  // and a reboot will boot into it. Thus, it's important that any future
  // errors in this script do not cause this script to return failure unless
  // in factory mode.

  // We have a new image, making the ureadahead pack files
  // out-of-date.  Delete the files so that ureadahead will
  // regenerate them on the next reboot.
  // WARNING: This doesn't work with upgrade from USB, rather than full
  // install/recovery. We don't have support for it as it'll increase the
  // complexity here, and only developers do upgrade from USB.
  if (!RemovePackFiles("/var/lib/ureadahead")) {
    printf("RemovePackFiles Failed\n");
    if (is_factory_install)
      return false;
  }

  // Create a file indicating that the install is completed. The file
  // will be used in /sbin/chromeos_startup to run tasks on the next boot.
  // See comments above about removing ureadahead files.
  if (!Touch("/mnt/stateful_partition/.install_completed")) {
    printf("Touch(/mnt/stateful_partition/.install_completed) FAILED\n");
    if (is_factory_install)
      return false;
  }

  // In factory process, firmware is either pre-flashed or assigned by
  // mini-omaha server, and we don't want to try updates inside postinst.
  if (!is_factory_install && firmware_update) {
    if (!FirmwareUpdate(install_dir, is_update)) {
      // Note: This will only rollback the ChromeOS verified boot target.
      // The assumption is that systems running firmware autoupdate
      // are not running legacy (non-ChromeOS) firmware. If the firmware
      // updater crashes or writes corrupt data rather than gracefully
      // failing, we'll probably need to recover with a recovery image.
      printf("Rolling back update due to failure installing required "
             "firmware.\n");

      // In all these checks below, we continue even if there's a failure
      // so as to cleanup as much as possible.
      new_kern_successful = false;
      bool rollback_successful = true;
      result = cgpt_manager.SetSuccessful(new_kern_num, new_kern_successful);
      if (result != kCgptSuccess) {
        rollback_successful = false;
        printf("Unable to set successful to %d for kernel %d\n",
               new_kern_successful,
               new_kern_num);
      }

      numTries = 0;
      result = cgpt_manager.SetNumTriesLeft(new_kern_num, numTries);
      if (result != kCgptSuccess) {
        rollback_successful = false;
        printf("Unable to set NumTriesLeft to %d for kernel %d\n",
               numTries,
               new_kern_num);
      }

      int priority = 0;
      result = cgpt_manager.SetPriority(new_kern_num, priority);
      if (result != kCgptSuccess) {
        rollback_successful = false;
        printf("Unable to set Priority to %d for kernel %d\n",
               priority,
               new_kern_num);
      }

      if (rollback_successful)
        printf("Successfully updated GPT with all settings to rollback.\n");

      return false;
    }
  }

  printf("ChromeosChrootPostinst complete\n");
  return true;
}

// This program is called after an AutoUpdate or USB install. This script is
// a simple wrapper that performs the minimal setup necessary to run
// chromeos-chroot-postinst inside an install root chroot.

bool RunPostInstall(const string& install_dir,
                    const string& install_dev) {

  printf("RunPostInstall(%s, %s)\n",
         install_dir.c_str(), install_dev.c_str());

  string src_version = LsbReleaseValue("/etc/lsb-release",
                                       "CHROMEOS_RELEASE_VERSION");

  if (src_version.empty()) {
    printf("CHROMEOS_RELEASE_VERSION not found in /etc/lsb-release");
    return false;
  }

  // TODO(hungte) Currently we rely on tag file /root/.force_update_firmware in
  // source (signed) rootfs to decide if postinst should perform firmware
  // updates (the file can be toggled by signing system, using tag_image.sh).
  // If this is changed, or if we want to allow user overriding firmware updates
  // in postinst in future, we may provide an option (ex, --update_firmware).
  string tag_file = install_dir + "/root/.force_update_firmware";
  bool firmware_update = (access(tag_file.c_str(), 0) == 0);

  return ChromeosChrootPostinst(install_dir,
                                src_version,
                                firmware_update,
                                install_dev);
}
