# *******************************************************************************
# Copyright (c) 2025 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
#
override CPU=x86_64
override BUILD_TYPE=SO
override PROJECT=kso
override PROJECT_ROOT=$(PWD)

ifndef QCONFIG
QCONFIG=qconfig.mk
endif

include $(QCONFIG)

INSTALLDIR=/lib/dll

define PINFO
PINFO DESCRIPTION=io-sock module demo using kernel socket interface
endef

EXTRA_CLEAN+= $(PROJECT_ROOT)/mods-$(PROJECT).use

define MODULE_SPECIFIC_OPTIONS

This can specify any module information

endef

include devs/mods.mk
