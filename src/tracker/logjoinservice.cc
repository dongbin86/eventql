/*
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include <fnord/base/exception.h>
#include <fnord/base/inspect.h>
#include <fnord/base/stringutil.h>
#include <fnord/base/uri.h>
#include <fnord/base/wallclock.h>
#include "logjoinservice.h"

namespace cm {

LogJoinService::LogJoinService(
    fnord::thread::TaskScheduler* scheduler,
    LogJoinOutput output) :
    scheduler_(scheduler),
    out_(output),
    stream_clock_(0) {}

void LogJoinService::insertLogline(
    CustomerNamespace* customer,
    const std::string& log_line) {
  insertLogline(customer, fnord::DateTime(), log_line);
}

void LogJoinService::insertLogline(
    CustomerNamespace* customer,
    const fnord::DateTime& time,
    const std::string& log_line) {
  auto stream_time = streamTime(time);

  fnord::URI::ParamList params;
  fnord::URI::parseQueryString(log_line, &params);

  flush(stream_time);

  /* extract uid (userid) and eid (eventid) */
  std::string c;
  if (!fnord::URI::getParam(params, "c", &c)) {
    RAISE(kParseError, "c param is missing");
  }

  auto c_s = c.find("~");
  if (c_s == std::string::npos) {
    RAISE(kParseError, "c param is invalid");
  }

  std::string uid = c.substr(0, c_s);
  std::string eid = c.substr(c_s + 1, c.length() - c_s - 1);
  if (uid.length() == 0 || eid.length() == 0) {
    RAISE(kParseError, "c param is invalid");
  }

  /* extract the event type */
  std::string evtype;
  if (!fnord::URI::getParam(params, "e", &evtype) || evtype.length() != 1) {
    RAISE(kParseError, "e param is missing");
  }

  /* process event */
  switch (evtype[0]) {

    /* query event */
    case 'q': {
      TrackedQuery query;
      query.time = time;
      query.flushed = false;
      query.fromParams(params);
      insertQuery(customer, uid, eid, query);
      break;
    }

    /* item visit event */
    case 'v': {
      TrackedItemVisit visit;
      visit.time = time;
      visit.fromParams(params);
      insertItemVisit(customer, uid, eid, visit);
      break;
    }

    default:
      RAISE(kParseError, "invalid e param");

  }
}

void LogJoinService::insertQuery(
    CustomerNamespace* customer,
    const std::string& uid,
    const std::string& eid,
    const TrackedQuery& query) {
  auto session = findOrCreateSession(customer, uid);
  std::lock_guard<std::mutex> l(session->mutex, std::adopt_lock_t {});

  auto iter = session->queries.find(eid);
  if (iter == session->queries.end()) {
    session->queries.emplace(eid, query);
  } else {
    iter->second.merge(query);
  }

  if (query.time.unixMicros() > session->last_seen_unix_micros) {
    session->last_seen_unix_micros = query.time.unixMicros();
  }
}

void LogJoinService::insertItemVisit(
    CustomerNamespace* customer,
    const std::string& uid,
    const std::string& eid,
    const TrackedItemVisit& visit) {
  {
    auto session = findOrCreateSession(customer, uid);
    std::lock_guard<std::mutex> l(session->mutex, std::adopt_lock_t {});

    auto iter = session->item_visits.find(eid);
    if (iter == session->item_visits.end()) {
      session->item_visits.emplace(eid, visit);
    } else {
      iter->second.merge(visit);
    }

    if (visit.time.unixMicros() > session->last_seen_unix_micros) {
      session->last_seen_unix_micros = visit.time.unixMicros();
    }
  }

  out_.recordJoinedItemVisit(customer, uid, eid, visit);
}

bool LogJoinService::maybeFlushSession(
    const std::string uid,
    TrackedSession* session,
    const fnord::DateTime& stream_time) {
  if (static_cast<uint64_t>(stream_time) < session->last_seen_unix_micros) {
    return false;
  }

  auto tdiff =
      static_cast<uint64_t>(stream_time) -
      static_cast<uint64_t>(session->last_seen_unix_micros);
  bool do_flush =
      tdiff > kSessionIdleTimeoutSeconds * fnord::DateTime::kMicrosPerSecond;
  bool do_update = do_flush;

  std::vector<std::pair<std::string, TrackedQuery*>> flushed;
  for (auto& query_pair : session->queries) {
    const auto& eid = query_pair.first;
    auto& query = query_pair.second;

    if (!query.flushed && (
            static_cast<uint64_t>(stream_time) -
            static_cast<uint64_t>(query.time)) >
            kMaxQueryClickDelaySeconds * fnord::DateTime::kMicrosPerSecond) {
      query.flushed = true;
      do_update = true;
      flushed.emplace_back(eid, &query);
    }
  }

  if (do_update) {
    session->update();
  }

  for (const auto flushed_query : flushed) {
    out_.recordJoinedQuery(
        session->customer,
        uid,
        flushed_query.first,
        *flushed_query.second);
  }

  if (do_flush) {
    out_.recordJoinedSession(session->customer, uid, *session);
  }

  return do_flush;
}

void LogJoinService::flush(const fnord::DateTime& stream_time) {
  std::lock_guard<std::mutex> l1(sessions_mutex_);
  fnord::iputs("stream_time=$0 active_sessions=$1", stream_time, sessions_.size());

  for (auto iter = sessions_.begin(); iter != sessions_.end(); ++iter) {
    const auto& uid = iter->first;
    const auto& session = iter->second;

    std::lock_guard<std::mutex> l2(session->mutex);
    if (maybeFlushSession(uid, session.get(), stream_time)) {
      sessions_.erase(iter);
    }
  }
}

TrackedSession* LogJoinService::findOrCreateSession(
    CustomerNamespace* customer,
    const std::string& uid) {
  TrackedSession* session = nullptr;

  std::lock_guard<std::mutex> lock_holder(sessions_mutex_);

  auto siter = sessions_.find(uid);
  if (siter == sessions_.end()) {
    session = new TrackedSession();
    session->customer = customer;
    session->last_seen_unix_micros = 0;
    sessions_.emplace(uid, session);
  } else {
    session = siter->second.get();
  }

  session->mutex.lock();
  return session;
}

fnord::DateTime LogJoinService::streamTime(const fnord::DateTime& time) {
  std::lock_guard<std::mutex> l(stream_clock_mutex_);

  if (time > stream_clock_) {
    stream_clock_ = time;
  }

  return stream_clock_;
}

} // namespace cm
