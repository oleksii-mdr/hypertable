/** -*- c++ -*-
 * Copyright (C) 2008 Doug Judd (Zvents, Inc.)
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "Common/Compat.h"
#include <cassert>
#include <string>
#include <vector>

extern "C" {
#include <poll.h>
#include <string.h>
}

#include <boost/algorithm/string.hpp>

#include "Common/Error.h"
#include "Common/FileUtils.h"
#include "Common/md5.h"

#include "Hypertable/Lib/CommitLog.h"
#include "Hypertable/Lib/CommitLogReader.h"


#include "CellStoreV0.h"
#include "Global.h"
#include "MergeScanner.h"
#include "MetadataNormal.h"
#include "MetadataRoot.h"
#include "Range.h"


using namespace Hypertable;
using namespace std;


Range::Range(MasterClientPtr &master_client_ptr,
             const TableIdentifier *identifier, SchemaPtr &schema_ptr,
             const RangeSpec *range, const RangeState *state)
    : m_master_client_ptr(master_client_ptr), m_identifier(*identifier),
      m_schema(schema_ptr), m_maintenance_in_progress(false),
      m_revision(0), m_latest_revision(0),
      m_added_inserts(0), m_state(*state), m_error(Error::OK) {
  AccessGroup *ag;

  memset(m_added_deletes, 0, 3*sizeof(int64_t));

  if (m_state.soft_limit == 0 || m_state.soft_limit > Global::range_max_bytes)
    m_state.soft_limit = Global::range_max_bytes;

  m_start_row = range->start_row;
  m_end_row = range->end_row;

  m_name = format("%s[%s..%s]", identifier->name, range->start_row,
                  range->end_row);

  m_is_root = (m_identifier.id == 0 && *range->start_row == 0
      && !strcmp(range->end_row, Key::END_ROOT_ROW));

  m_column_family_vector.resize(m_schema->get_max_column_family_id() + 1);

  list<Schema::AccessGroup *> *aglist = m_schema->get_access_group_list();

  for (list<Schema::AccessGroup *>::iterator ag_it = aglist->begin();
       ag_it != aglist->end(); ++ag_it) {
    ag = new AccessGroup(identifier, m_schema, (*ag_it), range);
    m_access_group_map[(*ag_it)->name] = ag;
    m_access_group_vector.push_back(ag);
    for (list<Schema::ColumnFamily *>::iterator cf_it
         = (*ag_it)->columns.begin(); cf_it != (*ag_it)->columns.end(); ++cf_it)
      m_column_family_vector[(*cf_it)->id] = ag;
  }

  if (m_is_root) {
    MetadataRoot metadata(m_schema);
    load_cell_stores(&metadata);
  }
  else {
    MetadataNormal metadata(&m_identifier, m_end_row);
    load_cell_stores(&metadata);
  }

  std::cout << "Range object for " << m_name << " constructed\n";
  std::cout << *state << endl;

  return;
}


/**
 */
Range::~Range() {
  for (size_t i=0; i<m_access_group_vector.size(); i++)
    delete m_access_group_vector[i];
}



/**
 *
 */
void Range::load_cell_stores(Metadata *metadata) {
  int error;
  AccessGroup *ag;
  CellStorePtr cellstore;
  uint32_t csid;
  const char *base, *ptr, *end;
  std::vector<String> csvec;
  String ag_name;
  String files;
  String file_str;

  metadata->reset_files_scan();

  while (metadata->get_next_files(ag_name, files)) {
    csvec.clear();

    if ((ag = m_access_group_map[ag_name]) == 0) {
      HT_ERRORF("Unrecognized access group name '%s' found in METADATA for "
                "table '%s'", ag_name.c_str(), m_identifier.name);
      continue;
    }

    ptr = base = (const char *)files.c_str();
    end = base + strlen(base);
    while (ptr < end) {

      while (*ptr != ';' && ptr < end)
        ptr++;

      file_str = String(base, ptr-base);
      boost::trim(file_str);

      if (file_str[0] == '#')
        file_str = file_str.substr(1);

      if (file_str != "")
        csvec.push_back(file_str);

      ++ptr;
      base = ptr;
    }

    for (size_t i=0; i<csvec.size(); i++) {

      HT_INFOF("Loading CellStore %s", csvec[i].c_str());

      cellstore = new CellStoreV0(Global::dfs);

      if (!extract_csid_from_path(csvec[i], &csid)) {
        HT_ERRORF("Unable to extract cell store ID from path '%s'",
                  csvec[i].c_str());
        continue;
      }
      if ((error = cellstore->open(csvec[i].c_str(), m_start_row.c_str(),
          m_end_row.c_str())) != Error::OK) {
        // this should throw an exception
        HT_ERRORF("Problem opening cell store '%s', skipping...",
                  csvec[i].c_str());
        continue;
      }
      if ((error = cellstore->load_index()) != Error::OK) {
        // this should throw an exception
        HT_ERRORF("Problem loading index of cell store '%s', skipping...",
                  csvec[i].c_str());
        continue;
      }

      if (cellstore->get_revision() > m_latest_revision)
        m_latest_revision = cellstore->get_revision();

      ag->add_cell_store(cellstore, csid);
    }

  }

}



/**
 */
bool Range::extract_csid_from_path(String &path, uint32_t *csidp) {
  const char *base;

  if ((base = strrchr(path.c_str(), '/')) == 0 || strncmp(base, "/cs", 3))
    *csidp = 0;
  else
    *csidp = atoi(base+3);

  return true;
}


/**
 */
int Range::add(const Key &key, const ByteString value) {

  if (m_error)
    return m_error;

  if (key.column_family_code >= m_column_family_vector.size()) {
    HT_ERROR_OUT <<"Bad column family ("<< (int)key.column_family_code <<") in "
        << m_identifier << HT_END;
    return Error::RANGESERVER_INVALID_COLUMNFAMILY;
  }

  if (key.flag == FLAG_DELETE_ROW) {
    for (AccessGroupMap::iterator iter = m_access_group_map.begin();
         iter != m_access_group_map.end(); ++iter)
      (*iter).second->add(key, value);
  }
  else
    m_column_family_vector[key.column_family_code]->add(key, value);

  if (key.flag == FLAG_INSERT)
    m_added_inserts++;
  else
    m_added_deletes[key.flag]++;

  if (key.revision > m_revision)
    m_revision = key.revision;

  return Error::OK;
}



CellListScanner *Range::create_scanner(ScanContextPtr &scan_ctx) {
  bool return_deletes = scan_ctx->spec ? scan_ctx->spec->return_deletes : false;
  MergeScanner *mscanner = new MergeScanner(scan_ctx, return_deletes);

  for (AccessGroupMap::iterator iter = m_access_group_map.begin();
       iter != m_access_group_map.end(); ++iter) {
    if ((*iter).second->include_in_scan(scan_ctx))
      mscanner->add_scanner((*iter).second->create_scanner(scan_ctx));
  }
  return mscanner;
}


uint64_t Range::disk_usage() {
  uint64_t usage = 0;
  for (size_t i=0; i<m_access_group_vector.size(); i++)
    usage += m_access_group_vector[i]->disk_usage();
  return usage;
}



/**
 *
 */
void
Range::get_compaction_priority_data(
    CompactionPriorityData &priority_data_vector) {
  size_t next_slot = priority_data_vector.size();

  priority_data_vector.resize(priority_data_vector.size()
                              + m_access_group_vector.size());

  for (size_t i=0; i<m_access_group_vector.size(); i++) {
    m_access_group_vector[i]->get_compaction_priority_data(
        priority_data_vector[next_slot]);
    next_slot++;
  }
}


void Range::split() {
  String old_start_row;

  HT_ASSERT(m_maintenance_in_progress);
  HT_ASSERT(!m_is_root);

  try {

    switch (m_state.state) {

    case (RangeState::STEADY):
      split_install_log();

    case (RangeState::SPLIT_LOG_INSTALLED):
      split_compact_and_shrink();

    case (RangeState::SPLIT_SHRUNK):
      split_notify_master();

    }

  }
  catch (Exception &e) {
    m_maintenance_in_progress = false;
    throw;
  }

  HT_INFOF("Split Complete.  New Range end_row=%s", m_start_row.c_str());

  m_maintenance_in_progress = false;
}



/**
 */
void Range::split_install_log() {
  std::vector<String> split_rows;
  char md5DigestStr[33];

  for (size_t i=0; i<m_access_group_vector.size(); i++)
    m_access_group_vector[i]->get_split_rows(split_rows, false);

  /**
   * If we didn't get at least one row from each Access Group, then try again
   * the hard way (scans CellCache for middle row)
   */
  if (split_rows.size() < m_access_group_vector.size()) {
    for (size_t i=0; i<m_access_group_vector.size(); i++)
      m_access_group_vector[i]->get_split_rows(split_rows, true);
  }
  sort(split_rows.begin(), split_rows.end());

  /**
  cout << flush;
  cout << "thelma Dumping split rows for " << m_name << "\n";
  for (size_t i=0; i<split_rows.size(); i++)
    cout << "thelma Range::get_split_row [" << i << "] = " << split_rows[i]
         << "\n";
  cout << flush;
  */

  /**
   * If we still didn't get a good split row, try again the *really* hard way
   * by collecting all of the cached rows, sorting them and then taking the
   * middle.
   */
  if (split_rows.size() > 0) {
    boost::mutex::scoped_lock lock(m_mutex);
    m_split_row = split_rows[split_rows.size()/2];
    if (m_split_row < m_start_row || m_split_row >= m_end_row) {
      split_rows.clear();
      for (size_t i=0; i<m_access_group_vector.size(); i++)
        m_access_group_vector[i]->get_cached_rows(split_rows);
      if (split_rows.size() > 0) {
        sort(split_rows.begin(), split_rows.end());
        m_split_row = split_rows[split_rows.size()/2];
        if (m_split_row < m_start_row || m_split_row >= m_end_row) {
          m_error = Error::RANGESERVER_ROW_OVERFLOW;
          HT_THROWF(Error::RANGESERVER_ROW_OVERFLOW,
                    "Unable to determine split row for range %s[%s..%s]",
                    m_identifier.name, m_start_row.c_str(), m_end_row.c_str());
        }
      }
      else {
        m_error = Error::RANGESERVER_ROW_OVERFLOW;
        HT_THROWF(Error::RANGESERVER_ROW_OVERFLOW,
                  "Unable to determine split row for range %s[%s..%s]",
                   m_identifier.name, m_start_row.c_str(), m_end_row.c_str());
      }
    }
  }
  else {
    m_error = Error::RANGESERVER_ROW_OVERFLOW;
    HT_THROWF(Error::RANGESERVER_ROW_OVERFLOW,
              "Unable to determine split row for range %s[%s..%s]",
              m_identifier.name, m_start_row.c_str(), m_end_row.c_str());
  }

  m_state.set_split_point(m_split_row);

  /**
   * Create split (transfer) log
   */
  md5_string(m_state.split_point, md5DigestStr);
  md5DigestStr[24] = 0;
  m_state.set_transfer_log(Global::log_dir + "/" + md5DigestStr);

  // Create transfer log dir
  try {
    Global::log_dfs->rmdir(m_state.transfer_log);
    Global::log_dfs->mkdirs(m_state.transfer_log);
  }
  catch (Exception &e) {
    HT_ERRORF("Problem creating log directory '%s': %s",
              m_state.transfer_log, e.what());
    HT_ABORT;
  }

  /**
   * Create and install the split log
   */
  {
    RangeUpdateBarrier::ScopedActivator block_updates(m_update_barrier);
    boost::mutex::scoped_lock lock(m_mutex);
    for (size_t i=0; i<m_access_group_vector.size(); i++)
      m_access_group_vector[i]->initiate_compaction();
    m_split_log_ptr = new CommitLog(Global::dfs, m_state.transfer_log);
  }

  /**
   * Write SPLIT_START MetaLog entry
   */
  m_state.state = RangeState::SPLIT_LOG_INSTALLED;
  for (int i=0; true; i++) {
    try {
      Global::range_log->log_split_start(m_identifier,
          RangeSpec(m_start_row.c_str(), m_end_row.c_str()),
          RangeSpec(m_start_row.c_str(), m_state.split_point), m_state);
      break;
    }
    catch (Exception &e) {
      if (i<3) {
        HT_ERRORF("%s - %s", Error::get_text(e.code()), e.what());
        poll(0, 0, 5000);
        continue;
      }
      HT_ERRORF("Problem writing SPLIT_LOG_INSTALLED meta log entry for %s "
                "split-point='%s'", m_name.c_str(), m_state.split_point);
      HT_FATAL_OUT << e << HT_END;
    }
  }

  if (Global::crash_test)
    Global::crash_test->maybe_crash("split-1");

}



/**
 *
 */
void Range::split_compact_and_shrink() {
  int error;
  String old_start_row = m_start_row;

  /**
   * Perform major compactions
   */
  {
    for (size_t i=0; i<m_access_group_vector.size(); i++)
      m_access_group_vector[i]->run_compaction(true);
  }

  try {
    String files;
    String metadata_key_str;
    KeySpec key;

    TableMutatorPtr mutator_ptr = Global::metadata_table_ptr->create_mutator();

    /**
     * Shrink old range in METADATA by updating the 'StartRow' column.
     */
    metadata_key_str = String("") + (uint32_t)m_identifier.id + ":" + m_end_row;
    key.row = metadata_key_str.c_str();
    key.row_len = metadata_key_str.length();
    key.column_qualifier = 0;
    key.column_qualifier_len = 0;
    key.column_family = "StartRow";
    mutator_ptr->set(key, (uint8_t *)m_state.split_point,
                     strlen(m_state.split_point));

    /**
     * Create an entry for the new range
     */
    metadata_key_str = format("%u:%s", m_identifier.id, m_state.split_point);
    key.row = metadata_key_str.c_str();
    key.row_len = metadata_key_str.length();
    key.column_qualifier = 0;
    key.column_qualifier_len = 0;

    key.column_family = "StartRow";
    mutator_ptr->set(key, old_start_row.c_str(), old_start_row.length());

    key.column_family = "Files";
    for (size_t i=0; i<m_access_group_vector.size(); i++) {
      key.column_qualifier = m_access_group_vector[i]->get_name();
      key.column_qualifier_len = strlen(m_access_group_vector[i]->get_name());
      m_access_group_vector[i]->get_files(files);
      mutator_ptr->set(key, (uint8_t *)files.c_str(), files.length());
    }

    mutator_ptr->flush();

  }
  catch (Hypertable::Exception &e) {
    // TODO: propagate exception
    HT_ERRORF("Problem updating ROOT METADATA range (new=%s, existing=%s) - %s",
        m_state.split_point, m_end_row.c_str(), Error::get_text(e.code()));
    // need to unblock updates and then return error
    HT_ABORT;
  }

  /**
   *  Shrink the range
   */

  {
    RangeUpdateBarrier::ScopedActivator block_updates(m_update_barrier);
    boost::mutex::scoped_lock lock(m_mutex);

    // Shrink access groups
    m_start_row = m_state.split_point;
    m_name = String(m_identifier.name) + "[" + m_start_row + ".." + m_end_row
             + "]";
    m_split_row = "";
    for (size_t i=0; i<m_access_group_vector.size(); i++)
      m_access_group_vector[i]->shrink(m_start_row);

    // Close and uninstall split log
    if ((error = m_split_log_ptr->close()) != Error::OK) {
      HT_ERRORF("Problem closing split log '%s' - %s",
          m_split_log_ptr->get_log_dir().c_str(), Error::get_text(error));
    }
    m_split_log_ptr = 0;
  }

  /**
   * Write SPLIT_SHRUNK MetaLog entry
   */
  m_state.state = RangeState::SPLIT_SHRUNK;
  m_state.timestamp = 0;
  m_state.set_old_start_row(old_start_row);
  m_state.clear_split_point();

  for (int i=0; true; i++) {
    try {
      Global::range_log->log_split_shrunk(m_identifier,
          RangeSpec(m_start_row.c_str(), m_end_row.c_str()), m_state);
      break;
    }
    catch (Exception &e) {
      if (i<3) {
        HT_ERRORF("%s - %s", Error::get_text(e.code()), e.what());
        poll(0, 0, 5000);
        continue;
      }
      HT_ERRORF("Problem writing SPLIT_SHRUNK meta log entry for %s "
                "split-point='%s'", m_name.c_str(), m_state.split_point);
      HT_FATAL_OUT << e << HT_END;
    }
  }

  if (Global::crash_test)
    Global::crash_test->maybe_crash("split-2");

}


/**
 *
 */
void Range::split_notify_master() {
  int error;
  RangeSpec range;
  uint64_t soft_limit = m_state.soft_limit;

  range.start_row = m_state.old_start_row;
  range.end_row = m_start_row.c_str();

  // update the latest generation, this should probably be protected
  m_identifier.generation = m_schema->get_generation();

  HT_INFOF("Reporting newly split off range %s[%s..%s] to Master",
           m_identifier.name, range.start_row, range.end_row);

  if (soft_limit < Global::range_max_bytes) {
    soft_limit *= 2;
    if (soft_limit > Global::range_max_bytes)
      soft_limit = Global::range_max_bytes;
  }

  if ((error = m_master_client_ptr->report_split(&m_identifier, range,
      m_state.transfer_log, soft_limit)) != Error::OK) {
    HT_THROWF(error, "Problem reporting split (table=%s, start_row=%s, "
              "end_row=%s) to master.", m_identifier.name, range.start_row,
              range.end_row);
  }

  /**
   * NOTE: try the following crash and make sure that the master does
   * not try to load the range twice.
   */

  if (Global::crash_test)
    Global::crash_test->maybe_crash("split-3");

  m_state.soft_limit = soft_limit;

  /**
   * Write SPLIT_DONE MetaLog entry
   */
  m_state.state = RangeState::STEADY;
  m_state.clear_transfer_log();
  m_state.clear_old_start_row();

  for (int i=0; true; i++) {
    try {
      Global::range_log->log_split_done(m_identifier,
          RangeSpec(m_start_row.c_str(), m_end_row.c_str()));
      break;
    }
    catch (Exception &e) {
      if (i<2) {
        HT_ERRORF("%s - %s", Error::get_text(e.code()), e.what());
        poll(0, 0, 5000);
        continue;
      }
      HT_ERRORF("Problem writing SPLIT_DONE meta log entry for %s "
                "split-point='%s'", m_name.c_str(), m_state.split_point);
      HT_FATAL_OUT << e << HT_END;
    }
  }

}


void Range::compact(bool major) {
  run_compaction(major);
  m_maintenance_in_progress = false;
}


void Range::run_compaction(bool major) {

  {
    RangeUpdateBarrier::ScopedActivator block_updates(m_update_barrier);
    boost::mutex::scoped_lock lock(m_mutex);
    for (size_t i=0; i<m_access_group_vector.size(); i++) {
      if (major || m_access_group_vector[i]->needs_compaction())
        m_access_group_vector[i]->initiate_compaction();
    }
  }

  for (size_t i=0; i<m_access_group_vector.size(); i++)
    m_access_group_vector[i]->run_compaction(major);
}


void Range::post_replay() {
  if (m_state.state == RangeState::SPLIT_LOG_INSTALLED) {
    CommitLogReaderPtr commit_log_reader_ptr =
        new CommitLogReader(Global::dfs, m_state.transfer_log);
    replay_transfer_log(commit_log_reader_ptr.get());
    commit_log_reader_ptr = 0;

    // re-initiate compaction
    for (size_t i=0; i<m_access_group_vector.size(); i++)
      m_access_group_vector[i]->initiate_compaction();

    m_split_log_ptr = new CommitLog(Global::dfs, m_state.transfer_log);
    m_split_row = m_state.split_point;
    HT_INFOF("Restored range state to SPLIT_LOG_INSTALLED (split point='%s' "
             "split log='%s')", m_state.split_point, m_state.transfer_log);
  }
}


void Range::dump_stats() {
  String range_str = (String)m_identifier.name + "[" + m_start_row + ".."
                      + m_end_row + "]";
  uint64_t collisions = 0;
  uint64_t cached = 0;
  for (size_t i=0; i<m_access_group_vector.size(); i++) {
    collisions += m_access_group_vector[i]->get_collision_count();
    cached += m_access_group_vector[i]->get_cached_count();
  }
  cout << "STAT\t" << range_str << "\tadded inserts\t" << m_added_inserts
       << endl;
  cout << "STAT\t" << range_str << "\tadded row deletes\t" << m_added_deletes[0]
       << endl;
  cout << "STAT\t" << range_str << "\tadded cf deletes\t" << m_added_deletes[1]
       << endl;
  cout << "STAT\t" << range_str << "\tadded cell deletes\t"
       << m_added_deletes[2] << endl;
  cout << "STAT\t" << range_str << "\tadded total\t"
       << (m_added_inserts + m_added_deletes[0] + m_added_deletes[1]
           + m_added_deletes[2]) << endl;
  cout << "STAT\t" << range_str << "\tcollisions\t" << collisions << endl;
  cout << "STAT\t" << range_str << "\tcached\t" << cached << endl;
  cout << flush;
}

void Range::get_statistics(RangeStat *stat) {
  uint64_t collisions = 0;
  uint64_t cached = 0;
  uint64_t disk_usage = 0;
  uint64_t memory_usage = 0;

  stat->added_inserts = m_added_inserts;
  stat->added_deletes[0] = m_added_deletes[0];
  stat->added_deletes[1] = m_added_deletes[1];
  stat->added_deletes[2] = m_added_deletes[2];

  for (size_t i=0; i<m_access_group_vector.size(); i++) {
    collisions += m_access_group_vector[i]->get_collision_count();
    cached += m_access_group_vector[i]->get_cached_count();
    disk_usage += m_access_group_vector[i]->disk_usage();
    memory_usage += m_access_group_vector[i]->memory_usage();
  }

  stat->collided_cells = collisions;
  stat->cached_cells = cached;
  stat->disk_usage = disk_usage;
  stat->memory_usage = memory_usage;



  {
    boost::mutex::scoped_lock lock(m_mutex);

    stat->table_identifier = m_identifier;

    RangeSpec spec;
    // these may change during a shrink
    spec.start_row = m_start_row.c_str();
    spec.end_row = m_end_row.c_str();

    stat->range_spec = spec;
  }
}


void Range::lock() {
  for (AccessGroupMap::iterator iter = m_access_group_map.begin();
       iter != m_access_group_map.end(); ++iter)
    (*iter).second->lock();
  m_revision = 0;
}


void Range::unlock() {

  {
    boost::mutex::scoped_lock lock(m_mutex);
    if (m_revision > m_latest_revision)
      m_latest_revision = m_revision;
  }

  for (AccessGroupMap::iterator iter = m_access_group_map.begin();
       iter != m_access_group_map.end(); ++iter)
    (*iter).second->unlock();
}



/**
 */
void Range::replay_transfer_log(CommitLogReader *commit_log_reader) {
  BlockCompressionHeaderCommitLog header;
  const uint8_t *base, *ptr, *end;
  size_t len;
  ByteString key, value;
  Key key_comps;
  size_t nblocks = 0;
  size_t count = 0;
  TableIdentifier table_id;

  m_revision = 0;

  try {

    while (commit_log_reader->next(&base, &len, &header)) {

      ptr = base;
      end = base + len;

      table_id.decode(&ptr, &len);

      if (strcmp(m_identifier.name, table_id.name))
        HT_THROWF(Error::RANGESERVER_CORRUPT_COMMIT_LOG,
                  "Table name mis-match in split log replay \"%s\" != \"%s\"",
                  m_identifier.name, table_id.name);

      while (ptr < end) {
        key.ptr = (uint8_t *)ptr;
        key_comps.load(key);
        ptr += key_comps.length;
        value.ptr = (uint8_t *)ptr;
        ptr += value.length();
        add(key_comps, value);
        count++;
      }
      nblocks++;
    }

    if (m_revision > m_latest_revision)
      m_latest_revision = m_revision;

    {
      boost::mutex::scoped_lock lock(m_mutex);
      HT_INFOF("Replayed %d updates (%d blocks) from split log '%s' into "
               "%s[%s..%s]", (int)count, (int)nblocks,
               commit_log_reader->get_log_dir().c_str(),
               m_identifier.name, m_start_row.c_str(), m_end_row.c_str());
    }

    m_added_inserts = 0;
    memset(m_added_deletes, 0, 3*sizeof(int64_t));

  }
  catch (Hypertable::Exception &e) {
    HT_ERRORF("Problem replaying split log - %s '%s'",
              Error::get_text(e.code()), e.what());
    if (m_revision > m_latest_revision)
      m_latest_revision = m_revision;
    throw;
  }
}



/**
 */
int64_t Range::get_scan_revision() {
  boost::mutex::scoped_lock lock(m_mutex);
  return m_latest_revision;
}
