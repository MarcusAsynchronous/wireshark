# Wireshark - Network traffic analyzer
# By Gerald Combs <gerald@wireshark.org>
# Copyright 1998 Gerald Combs
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

include ../Makefile.inc

PROTOCOL_NAME = isdn-sup

EXPORT_FILES = $(PROTOCOL_NAME)-exp.cnf

EXT_ASN_FILE_LIST =

ASN_FILE_LIST = \
	Addressing-Data-Elements.asn \
	Basic-Service-Elements.asn \
	Embedded-Q931-Types.asn \
	General-Errors.asn \
	Advice-of-Charge-Operations.asn \
	Closed-User-Group-Service-Operations.asn \
	Conference-Add-On-Operations.asn \
	Diversion-Operations.asn \
	MCID-Operations.asn \
	User-To-User-Signalling-Operations.asn \
	Freephone-Operations.asn \
	MLPP-Operations-And-Errors.asn

INCLUDED_ASN_FILE_LIST = \
	CCBS-Operations-and-Errors.asn \
	CCBS-private-networks-Operations-and-Errors.asn \
	Explicit-Call-Transfer-Operations-and-Errors.asn \
	MWI-Operations-and-Errors.asn \
	Outgoing-Call-Barring-Operations-and-Errors.asn \
	Set-Operations-and-Errors.asn \
	Three-Party-Operations.asn

# The packet-$(PROTOCOL_NAME)-template.h and $(PROTOCOL_NAME).asn
# files do not exist for all protocols: Please add/remove as required.
EXTRA_DIST = \
	$(EXTRA_DIST_COMMON) \
	$(ASN_FILE_LIST) \
	$(INCLUDED_ASN_FILE_LIST) \
	packet-$(PROTOCOL_NAME)-template.c \
	$(PROTOCOL_NAME).cnf

SRC_FILES = \
	$(EXTRA_DIST) \
	$(EXT_ASN_FILE_LIST)

A2W_FLAGS= -b

EXTRA_CNF =
