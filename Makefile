# Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

CFLAGS := -I$(SRC)/include $(CFLAGS) -I/usr/include/verity
CXXFLAGS := -I$(SRC)/include -DCHROMEOS_ENVIRONMENT $(CXXFLAGS)
CXXFLAGS += -I/usr/include/verity

LDFLAGS += -ldump_kernel_config -lcgpt-cc -ldm-bht

CXX_STATIC_BINARY(cros_installer): \
		$(C_OBJECTS) \
		$(filter-out %_testrunner.o %_unittest.o,$(CXX_OBJECTS))

clean: CLEAN(cros_installer)
all: CXX_STATIC_BINARY(cros_installer)
# Convenience target.
cros_installer: CXX_STATIC_BINARY(cros_installer)

CXX_BINARY(cros_installer_test): \
		$(C_OBJECTS) \
		$(filter-out %_testrunner.o %_unittest.o,$(CXX_OBJECTS))

clean: CLEAN(cros_installer_test)

all: CXX_BINARY(cros_installer_test)

TEST(cros_installer): GTEST_ARGS = --test
tests: TEST(cros_installer) TEST(CXX_BINARY(cros_installer_test))
