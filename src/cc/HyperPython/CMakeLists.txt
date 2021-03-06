#
# Copyright (C) 2007-2012 Hypertable, Inc.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 3
# of the License, or any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.
#
# Python bindings for SerializedCellsReader/SerializedCellsWriter
#

set(CMAKE_CXX_FLAGS -DHAVE_NETINET_IN_H)

include_directories(${PYTHON_INCLUDE_DIR})

add_library(HyperPython SHARED SerializedCells.cc)
target_link_libraries(HyperPython HyperThrift HyperCommon
    Hypertable ${BOOST_PYTHON_LIB} ${PYTHON_LIB_DIR} ${PYTHON_LIBRARY} ${Thrift_LIBS}
    ${LibEvent_LIBS})
install(TARGETS HyperPython
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION "lib/py/hypertable"
        ARCHIVE DESTINATION "lib/py/hypertable")

add_test(HyperPython-reader env PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}:${CMAKE_SOURCE_DIR}/src/py/ThriftClient:${CMAKE_SOURCE_DIR}/src/py/ThriftClient/gen-py
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-reader.sh)

add_test(HyperPython-writer env PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}:${CMAKE_SOURCE_DIR}/src/py/ThriftClient:${CMAKE_SOURCE_DIR}/src/py/ThriftClient/gen-py
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-writer.sh)
