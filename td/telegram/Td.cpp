//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/Td.h"

#include "td/db/binlog/BinlogEvent.h"

#include "td/telegram/net/ConnectionCreator.h"
#include "td/telegram/net/MtprotoHeader.h"
#include "td/telegram/net/NetQuery.h"
#include "td/telegram/net/NetQueryDelayer.h"
#include "td/telegram/net/NetQueryDispatcher.h"
#include "td/telegram/net/NetStatsManager.h"
#include "td/telegram/net/TempAuthKeyWatchdog.h"

#include "td/telegram/AccessRights.h"
#include "td/telegram/AnimationsManager.h"
#include "td/telegram/AudiosManager.h"
#include "td/telegram/AuthManager.h"
#include "td/telegram/CallbackQueriesManager.h"
#include "td/telegram/CallId.h"
#include "td/telegram/CallManager.h"
#include "td/telegram/ChannelId.h"
#include "td/telegram/ChatId.h"
#include "td/telegram/ConfigManager.h"
#include "td/telegram/ConfigShared.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/DeviceTokenManager.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/DialogParticipant.h"
#include "td/telegram/DocumentsManager.h"
#include "td/telegram/files/FileGcParameters.h"
#include "td/telegram/files/FileId.h"
#include "td/telegram/files/FileManager.h"
#include "td/telegram/Global.h"
#include "td/telegram/HashtagHints.h"
#include "td/telegram/InlineQueriesManager.h"
#include "td/telegram/MessageEntity.h"
#include "td/telegram/MessageId.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/misc.h"
#include "td/telegram/PasswordManager.h"
#include "td/telegram/PrivacyManager.h"
#include "td/telegram/SecretChatId.h"
#include "td/telegram/SecretChatsManager.h"
#include "td/telegram/StateManager.h"
#include "td/telegram/StickersManager.h"
#include "td/telegram/StorageManager.h"
#include "td/telegram/TdDb.h"
#include "td/telegram/TopDialogManager.h"
#include "td/telegram/UpdatesManager.h"
#include "td/telegram/VideoNotesManager.h"
#include "td/telegram/VideosManager.h"
#include "td/telegram/VoiceNotesManager.h"
#include "td/telegram/WebPageId.h"
#include "td/telegram/WebPagesManager.h"

#include "td/telegram/td_api.hpp"
#include "td/telegram/telegram_api.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/mtproto/utils.h"  // for create_storer, fetch_result, etc, TODO

#include "td/utils/buffer.h"
#include "td/utils/format.h"
#include "td/utils/MimeType.h"
#include "td/utils/misc.h"
#include "td/utils/PathView.h"
#include "td/utils/port/path.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/Timer.h"
#include "td/utils/tl_parsers.h"
#include "td/utils/utf8.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <type_traits>

namespace td {
namespace {
DbKey as_db_key(string key) {
  // Database will still be effectively not encrypted, but
  // 1. sqlite db will be protected from corruption, because that's how sqlcipher works
  // 2. security through obscurity
  // 3. no need for reencryption of sqlite db
  if (key.empty()) {
    return DbKey::raw_key("cucumber");
  }
  return DbKey::raw_key(std::move(key));
}
}  // namespace

void Td::ResultHandler::set_td(Td *new_td) {
  CHECK(td == nullptr);
  td = new_td;
}

void Td::ResultHandler::on_result(NetQueryPtr query) {
  CHECK(query->is_ready());
  if (query->is_ok()) {
    on_result(query->id(), std::move(query->ok()));
  } else {
    on_error(query->id(), std::move(query->error()));
  }
  query->clear();
}

void Td::ResultHandler::send_query(NetQueryPtr query) {
  td->add_handler(query->id(), shared_from_this());
  td->send(std::move(query));
}

class GetNearestDcQuery : public Td::ResultHandler {
 public:
  void send() {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::help_getNearestDc()), DcId::main(),
                                               NetQuery::Type::Common, NetQuery::AuthFlag::Off));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::help_getNearestDc>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }
  }

  void on_error(uint64 id, Status status) override {
    LOG(ERROR) << "GetNearestDc returned " << status;
    status.ignore();
  }
};

class GetWallpapersQuery : public Td::ResultHandler {
  Promise<tl_object_ptr<td_api::wallpapers>> promise_;

 public:
  explicit GetWallpapersQuery(Promise<tl_object_ptr<td_api::wallpapers>> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::account_getWallPapers())));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::account_getWallPapers>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto wallpapers = result_ptr.move_as_ok();

    auto results = make_tl_object<td_api::wallpapers>();
    results->wallpapers_.reserve(wallpapers.size());
    for (auto &wallpaper_ptr : wallpapers) {
      CHECK(wallpaper_ptr != nullptr);
      switch (wallpaper_ptr->get_id()) {
        case telegram_api::wallPaper::ID: {
          auto wallpaper = move_tl_object_as<telegram_api::wallPaper>(wallpaper_ptr);
          vector<tl_object_ptr<td_api::photoSize>> sizes;
          sizes.reserve(wallpaper->sizes_.size());
          for (auto &size_ptr : wallpaper->sizes_) {
            auto photo_size =
                get_photo_size(td->file_manager_.get(), FileType::Wallpaper, 0, 0, DialogId(), std::move(size_ptr));
            sizes.push_back(get_photo_size_object(td->file_manager_.get(), &photo_size));
          }
          results->wallpapers_.push_back(
              make_tl_object<td_api::wallpaper>(wallpaper->id_, std::move(sizes), wallpaper->color_));
          break;
        }
        case telegram_api::wallPaperSolid::ID: {
          auto wallpaper = move_tl_object_as<telegram_api::wallPaperSolid>(wallpaper_ptr);
          results->wallpapers_.push_back(make_tl_object<td_api::wallpaper>(
              wallpaper->id_, vector<tl_object_ptr<td_api::photoSize>>(), wallpaper->bg_color_));
          break;
        }
        default:
          UNREACHABLE();
      }
    }
    promise_.set_value(std::move(results));
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class GetRecentMeUrlsQuery : public Td::ResultHandler {
  Promise<tl_object_ptr<td_api::tMeUrls>> promise_;

 public:
  explicit GetRecentMeUrlsQuery(Promise<tl_object_ptr<td_api::tMeUrls>> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &referrer) {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::help_getRecentMeUrls(referrer))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::help_getRecentMeUrls>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    auto urls_full = result_ptr.move_as_ok();
    td->contacts_manager_->on_get_users(std::move(urls_full->users_));
    td->contacts_manager_->on_get_chats(std::move(urls_full->chats_));

    auto urls = std::move(urls_full->urls_);
    auto results = make_tl_object<td_api::tMeUrls>();
    results->urls_.reserve(urls.size());
    for (auto &url_ptr : urls) {
      CHECK(url_ptr != nullptr);
      tl_object_ptr<td_api::tMeUrl> result = make_tl_object<td_api::tMeUrl>();
      switch (url_ptr->get_id()) {
        case telegram_api::recentMeUrlUser::ID: {
          auto url = move_tl_object_as<telegram_api::recentMeUrlUser>(url_ptr);
          result->url_ = std::move(url->url_);
          UserId user_id(url->user_id_);
          if (!user_id.is_valid()) {
            LOG(ERROR) << "Receive invalid " << user_id;
            result = nullptr;
            break;
          }
          result->type_ = make_tl_object<td_api::tMeUrlTypeUser>(
              td->contacts_manager_->get_user_id_object(user_id, "tMeUrlTypeUser"));
          break;
        }
        case telegram_api::recentMeUrlChat::ID: {
          auto url = move_tl_object_as<telegram_api::recentMeUrlChat>(url_ptr);
          result->url_ = std::move(url->url_);
          ChannelId channel_id(url->chat_id_);
          if (!channel_id.is_valid()) {
            LOG(ERROR) << "Receive invalid " << channel_id;
            result = nullptr;
            break;
          }
          result->type_ = make_tl_object<td_api::tMeUrlTypeSupergroup>(
              td->contacts_manager_->get_supergroup_id_object(channel_id, "tMeUrlTypeSupergroup"));
          break;
        }
        case telegram_api::recentMeUrlChatInvite::ID: {
          auto url = move_tl_object_as<telegram_api::recentMeUrlChatInvite>(url_ptr);
          result->url_ = std::move(url->url_);
          td->contacts_manager_->on_get_dialog_invite_link_info(result->url_, std::move(url->chat_invite_));
          result->type_ = make_tl_object<td_api::tMeUrlTypeChatInvite>(
              td->contacts_manager_->get_chat_invite_link_info_object(result->url_));
          break;
        }
        case telegram_api::recentMeUrlStickerSet::ID: {
          auto url = move_tl_object_as<telegram_api::recentMeUrlStickerSet>(url_ptr);
          result->url_ = std::move(url->url_);
          auto sticker_set_id = td->stickers_manager_->on_get_sticker_set_covered(std::move(url->set_), false);
          if (sticker_set_id == 0) {
            LOG(ERROR) << "Receive invalid sticker set";
            result = nullptr;
            break;
          }
          result->type_ = make_tl_object<td_api::tMeUrlTypeStickerSet>(sticker_set_id);
          break;
        }
        case telegram_api::recentMeUrlUnknown::ID:
          // skip
          result = nullptr;
          break;
        default:
          UNREACHABLE();
      }
      if (result != nullptr) {
        results->urls_.push_back(std::move(result));
      }
    }
    promise_.set_value(std::move(results));
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class SendCustomRequestQuery : public Td::ResultHandler {
  Promise<string> promise_;

 public:
  explicit SendCustomRequestQuery(Promise<string> &&promise) : promise_(std::move(promise)) {
  }

  void send(const string &method, const string &parameters) {
    send_query(G()->net_query_creator().create(create_storer(
        telegram_api::bots_sendCustomRequest(method, make_tl_object<telegram_api::dataJSON>(parameters)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::bots_sendCustomRequest>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    promise_.set_value(std::move(result_ptr.ok()->data_));
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class AnswerCustomQueryQuery : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit AnswerCustomQueryQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int64 custom_query_id, const string &data) {
    send_query(G()->net_query_creator().create(create_storer(
        telegram_api::bots_answerWebhookJSONQuery(custom_query_id, make_tl_object<telegram_api::dataJSON>(data)))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::bots_answerWebhookJSONQuery>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      LOG(INFO) << "Sending answer to a custom query has failed";
    }
    promise_.set_value(Unit());
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class SetBotUpdatesStatusQuery : public Td::ResultHandler {
 public:
  void send(int32 pending_update_count, const string &error_message) {
    send_query(G()->net_query_creator().create(
        create_storer(telegram_api::help_setBotUpdatesStatus(pending_update_count, error_message))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::help_setBotUpdatesStatus>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG_IF(WARNING, !result) << "Set bot updates status has failed";
  }

  void on_error(uint64 id, Status status) override {
    LOG(WARNING) << "Receive error for SetBotUpdatesStatus: " << status;
    status.ignore();
  }
};

class UpdateStatusQuery : public Td::ResultHandler {
 public:
  void send(bool is_offline) {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::account_updateStatus(is_offline))));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::account_updateStatus>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    LOG(INFO) << "UpdateStatus returned " << result;
  }

  void on_error(uint64 id, Status status) override {
    LOG(ERROR) << "Receive error for UpdateStatusQuery: " << status;
    status.ignore();
  }
};

class GetInviteTextQuery : public Td::ResultHandler {
  Promise<string> promise_;

 public:
  explicit GetInviteTextQuery(Promise<string> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::help_getInviteText())));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::help_getInviteText>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    promise_.set_value(std::move(result_ptr.ok()->message_));
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

class GetTermsOfServiceQuery : public Td::ResultHandler {
  Promise<string> promise_;

 public:
  explicit GetTermsOfServiceQuery(Promise<string> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::help_getTermsOfService())));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::help_getTermsOfService>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, result_ptr.move_as_error());
    }

    promise_.set_value(std::move(result_ptr.ok()->text_));
  }

  void on_error(uint64 id, Status status) override {
    promise_.set_error(std::move(status));
  }
};

template <class T = Unit>
class RequestActor : public Actor {
 public:
  RequestActor(ActorShared<Td> td_id, uint64 request_id)
      : td_id_(std::move(td_id)), td(td_id_.get().get_actor_unsafe()), request_id_(request_id) {
  }

  void loop() override {
    PromiseActor<T> promise;
    FutureActor<T> future;
    init_promise_future(&promise, &future);

    do_run(PromiseCreator::from_promise_actor(std::move(promise)));

    if (future.is_ready()) {
      if (future.is_error()) {
        do_send_error(future.move_as_error());
      } else {
        do_set_result(future.move_as_ok());
        do_send_result();
      }
      stop();
    } else {
      if (--tries_left_ == 0) {
        future.close();
        do_send_error(Status::Error(400, "Requested data is unaccessible"));
        return stop();
      }

      future.set_event(EventCreator::raw(actor_id(), nullptr));
      future_ = std::move(future);
    }
  }

  void raw_event(const Event::Raw &event) override {
    if (future_.is_error()) {
      auto error = future_.move_as_error();
      if (error == Status::Hangup()) {
        // dropping query due to lost authorization or lost promise
        // td may be already closed, so we should check is auth_manager_ is empty
        bool is_authorized = td->auth_manager_ && td->auth_manager_->is_authorized();
        if (is_authorized) {
          LOG(ERROR) << "Promise was lost";
          do_send_error(Status::Error(500, "Query can't be answered due to bug in the TDLib"));
        } else {
          do_send_error(Status::Error(401, "Unauthorized"));
        }
        return stop();
      }

      do_send_error(std::move(error));
      stop();
    } else {
      do_set_result(future_.move_as_ok());
      loop();
    }
  }

  void on_start_migrate(int32 /*sched_id*/) override {
    UNREACHABLE();
  }
  void on_finish_migrate() override {
    UNREACHABLE();
  }

  int get_tries() const {
    return tries_left_;
  }

  void set_tries(int32 tries) {
    tries_left_ = tries;
  }

 protected:
  ActorShared<Td> td_id_;
  Td *td;

  void send_result(tl_object_ptr<td_api::Object> &&result) {
    send_closure(td_id_, &Td::send_result, request_id_, std::move(result));
  }

  void send_error(Status &&status) {
    LOG(INFO) << "Receive error for query: " << status;
    send_closure(td_id_, &Td::send_error, request_id_, std::move(status));
  }

 private:
  virtual void do_run(Promise<T> &&promise) = 0;

  virtual void do_send_result() {
    send_result(make_tl_object<td_api::ok>());
  }

  virtual void do_send_error(Status &&status) {
    send_error(std::move(status));
  }

  virtual void do_set_result(T &&result) {
    CHECK((std::is_same<T, Unit>::value));  // all other results should be implicitly handled by overriding this method
  }

  void hangup() override {
    do_send_error(Status::Error(500, "Request aborted"));
    stop();
  }

  friend class RequestOnceActor;

  uint64 request_id_;
  int tries_left_ = 2;
  FutureActor<T> future_;
};

class RequestOnceActor : public RequestActor<> {
 public:
  RequestOnceActor(ActorShared<Td> td_id, uint64 request_id) : RequestActor(std::move(td_id), request_id) {
  }

  void loop() override {
    if (get_tries() < 2) {
      do_send_result();
      stop();
      return;
    }

    RequestActor::loop();
  }
};

/*** Td ***/
/** Td queries **/
class TestQuery : public Td::ResultHandler {
 public:
  explicit TestQuery(uint64 request_id) : request_id_(request_id) {
  }

  void send() {
    send_query(G()->net_query_creator().create(create_storer(telegram_api::help_getConfig())));
  }

  void on_result(uint64 id, BufferSlice packet) override {
    auto result_ptr = fetch_result<telegram_api::help_getConfig>(packet);
    if (result_ptr.is_error()) {
      return on_error(id, Status::Error(500, "Fetch failed"));
    }

    LOG(DEBUG) << "TestOK: " << to_string(result_ptr.ok());
    send_closure(G()->td(), &Td::send_result, request_id_, make_tl_object<td_api::ok>());
  }

  void on_error(uint64 id, Status status) override {
    status.ignore();
    LOG(ERROR) << "Test query failed: " << status;
  }

 private:
  uint64 request_id_;
};

class GetAccountTtlRequest : public RequestActor<int32> {
  int32 account_ttl_;

  void do_run(Promise<int32> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(account_ttl_));
      return;
    }

    td->contacts_manager_->get_account_ttl(std::move(promise));
  }

  void do_set_result(int32 &&result) override {
    account_ttl_ = result;
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::accountTtl>(account_ttl_));
  }

 public:
  GetAccountTtlRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class SetAccountTtlRequest : public RequestOnceActor {
  int32 account_ttl_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_account_ttl(account_ttl_, std::move(promise));
  }

 public:
  SetAccountTtlRequest(ActorShared<Td> td, uint64 request_id, int32 account_ttl)
      : RequestOnceActor(std::move(td), request_id), account_ttl_(account_ttl) {
  }
};

class GetActiveSessionsRequest : public RequestActor<tl_object_ptr<td_api::sessions>> {
  tl_object_ptr<td_api::sessions> sessions_;

  void do_run(Promise<tl_object_ptr<td_api::sessions>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(sessions_));
      return;
    }

    td->contacts_manager_->get_active_sessions(std::move(promise));
  }

  void do_set_result(tl_object_ptr<td_api::sessions> &&result) override {
    sessions_ = std::move(result);
  }

  void do_send_result() override {
    CHECK(sessions_ != nullptr);
    send_result(std::move(sessions_));
  }

 public:
  GetActiveSessionsRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class TerminateSessionRequest : public RequestOnceActor {
  int64 session_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->terminate_session(session_id_, std::move(promise));
  }

 public:
  TerminateSessionRequest(ActorShared<Td> td, uint64 request_id, int64 session_id)
      : RequestOnceActor(std::move(td), request_id), session_id_(session_id) {
  }
};

class TerminateAllOtherSessionsRequest : public RequestOnceActor {
  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->terminate_all_other_sessions(std::move(promise));
  }

 public:
  TerminateAllOtherSessionsRequest(ActorShared<Td> td, uint64 request_id)
      : RequestOnceActor(std::move(td), request_id) {
  }
};

class GetUserRequest : public RequestActor<> {
  UserId user_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->get_user(user_id_, get_tries(), std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_user_object(user_id_));
  }

 public:
  GetUserRequest(ActorShared<Td> td, uint64 request_id, int32 user_id)
      : RequestActor(std::move(td), request_id), user_id_(user_id) {
    set_tries(3);
  }
};

class GetUserFullInfoRequest : public RequestActor<> {
  UserId user_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->get_user_full(user_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_user_full_info_object(user_id_));
  }

 public:
  GetUserFullInfoRequest(ActorShared<Td> td, uint64 request_id, int32 user_id)
      : RequestActor(std::move(td), request_id), user_id_(user_id) {
  }
};

class GetGroupRequest : public RequestActor<> {
  ChatId chat_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->get_chat(chat_id_, get_tries(), std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_basic_group_object(chat_id_));
  }

 public:
  GetGroupRequest(ActorShared<Td> td, uint64 request_id, int32 chat_id)
      : RequestActor(std::move(td), request_id), chat_id_(chat_id) {
    set_tries(3);
  }
};

class GetGroupFullInfoRequest : public RequestActor<> {
  ChatId chat_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->get_chat_full(chat_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_basic_group_full_info_object(chat_id_));
  }

 public:
  GetGroupFullInfoRequest(ActorShared<Td> td, uint64 request_id, int32 chat_id)
      : RequestActor(std::move(td), request_id), chat_id_(chat_id) {
  }
};

class GetSupergroupRequest : public RequestActor<> {
  ChannelId channel_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->get_channel(channel_id_, get_tries(), std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_supergroup_object(channel_id_));
  }

 public:
  GetSupergroupRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id)
      : RequestActor(std::move(td), request_id), channel_id_(channel_id) {
    set_tries(3);
  }
};

class GetSupergroupFullInfoRequest : public RequestActor<> {
  ChannelId channel_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->get_channel_full(channel_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_channel_full_info_object(channel_id_));
  }

 public:
  GetSupergroupFullInfoRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id)
      : RequestActor(std::move(td), request_id), channel_id_(channel_id) {
  }
};

class GetSecretChatRequest : public RequestActor<> {
  SecretChatId secret_chat_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->get_secret_chat(secret_chat_id_, get_tries() < 2, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_secret_chat_object(secret_chat_id_));
  }

 public:
  GetSecretChatRequest(ActorShared<Td> td, uint64 request_id, int32 secret_chat_id)
      : RequestActor(std::move(td), request_id), secret_chat_id_(secret_chat_id) {
  }
};

class GetChatRequest : public RequestActor<> {
  DialogId dialog_id_;

  bool dialog_found_ = false;

  void do_run(Promise<Unit> &&promise) override {
    dialog_found_ = td->messages_manager_->load_dialog(dialog_id_, get_tries(), std::move(promise));
  }

  void do_send_result() override {
    if (!dialog_found_) {
      send_error(Status::Error(400, "Chat is not accessible"));
    } else {
      send_result(td->messages_manager_->get_chat_object(dialog_id_));
    }
  }

 public:
  GetChatRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id)
      : RequestActor(std::move(td), request_id), dialog_id_(dialog_id) {
    set_tries(3);
  }
};

class GetChatsRequest : public RequestActor<> {
  DialogDate offset_;
  int32 limit_;

  vector<DialogId> dialog_ids_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_ids_ = td->messages_manager_->get_dialogs(offset_, limit_, get_tries() < 2, std::move(promise));
  }

  void do_send_result() override {
    send_result(MessagesManager::get_chats_object(dialog_ids_));
  }

 public:
  GetChatsRequest(ActorShared<Td> td, uint64 request_id, int64 offset_order, int64 offset_dialog_id, int32 limit)
      : RequestActor(std::move(td), request_id), offset_(offset_order, DialogId(offset_dialog_id)), limit_(limit) {
    // 1 for database + 1 for server request + 1 for server request at the end + 1 for return + 1 just in case
    set_tries(5);
  }
};

class SearchPublicChatRequest : public RequestActor<> {
  string username_;

  DialogId dialog_id_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_id_ = td->messages_manager_->search_public_dialog(username_, get_tries() < 3, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_chat_object(dialog_id_));
  }

 public:
  SearchPublicChatRequest(ActorShared<Td> td, uint64 request_id, string username)
      : RequestActor(std::move(td), request_id), username_(std::move(username)) {
    set_tries(3);
  }
};

class SearchPublicChatsRequest : public RequestActor<> {
  string query_;

  vector<DialogId> dialog_ids_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_ids_ = td->messages_manager_->search_public_dialogs(query_, std::move(promise));
  }

  void do_send_result() override {
    send_result(MessagesManager::get_chats_object(dialog_ids_));
  }

 public:
  SearchPublicChatsRequest(ActorShared<Td> td, uint64 request_id, string query)
      : RequestActor(std::move(td), request_id), query_(std::move(query)) {
  }
};

class SearchChatsRequest : public RequestActor<> {
  string query_;
  int32 limit_;

  vector<DialogId> dialog_ids_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_ids_ = td->messages_manager_->search_dialogs(query_, limit_, std::move(promise)).second;
  }

  void do_send_result() override {
    send_result(MessagesManager::get_chats_object(dialog_ids_));
  }

 public:
  SearchChatsRequest(ActorShared<Td> td, uint64 request_id, string query, int32 limit)
      : RequestActor(std::move(td), request_id), query_(std::move(query)), limit_(limit) {
  }
};

class SearchChatsOnServerRequest : public RequestActor<> {
  string query_;
  int32 limit_;

  vector<DialogId> dialog_ids_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_ids_ = td->messages_manager_->search_dialogs_on_server(query_, limit_, std::move(promise));
  }

  void do_send_result() override {
    send_result(MessagesManager::get_chats_object(dialog_ids_));
  }

 public:
  SearchChatsOnServerRequest(ActorShared<Td> td, uint64 request_id, string query, int32 limit)
      : RequestActor(std::move(td), request_id), query_(std::move(query)), limit_(limit) {
  }
};

class GetGroupsInCommonRequest : public RequestActor<> {
  UserId user_id_;
  DialogId offset_dialog_id_;
  int32 limit_;

  vector<DialogId> dialog_ids_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_ids_ = td->messages_manager_->get_common_dialogs(user_id_, offset_dialog_id_, limit_, get_tries() < 2,
                                                            std::move(promise));
  }

  void do_send_result() override {
    send_result(MessagesManager::get_chats_object(dialog_ids_));
  }

 public:
  GetGroupsInCommonRequest(ActorShared<Td> td, uint64 request_id, int32 user_id, int64 offset_dialog_id, int32 limit)
      : RequestActor(std::move(td), request_id), user_id_(user_id), offset_dialog_id_(offset_dialog_id), limit_(limit) {
  }
};

class GetCreatedPublicChatsRequest : public RequestActor<> {
  vector<DialogId> dialog_ids_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_ids_ = td->contacts_manager_->get_created_public_dialogs(std::move(promise));
  }

  void do_send_result() override {
    send_result(MessagesManager::get_chats_object(dialog_ids_));
  }

 public:
  GetCreatedPublicChatsRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class GetMessageRequest : public RequestOnceActor {
  FullMessageId full_message_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->get_message(full_message_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_message_object(full_message_id_));
  }

 public:
  GetMessageRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id)
      : RequestOnceActor(std::move(td), request_id), full_message_id_(DialogId(dialog_id), MessageId(message_id)) {
  }
};

class GetMessagesRequest : public RequestOnceActor {
  DialogId dialog_id_;
  vector<MessageId> message_ids_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->get_messages(dialog_id_, message_ids_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_messages_object(-1, dialog_id_, message_ids_));
  }

 public:
  GetMessagesRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, const vector<int64> &message_ids)
      : RequestOnceActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , message_ids_(MessagesManager::get_message_ids(message_ids)) {
  }
};

class GetPublicMessageLinkRequest : public RequestActor<> {
  FullMessageId full_message_id_;
  bool for_group_;

  string link_;
  string html_;

  void do_run(Promise<Unit> &&promise) override {
    std::tie(link_, html_) =
        td->messages_manager_->get_public_message_link(full_message_id_, for_group_, std::move(promise));
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::publicMessageLink>(link_, html_));
  }

 public:
  GetPublicMessageLinkRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id, bool for_group)
      : RequestActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , for_group_(for_group) {
  }
};

class EditMessageTextRequest : public RequestOnceActor {
  FullMessageId full_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;
  tl_object_ptr<td_api::InputMessageContent> input_message_content_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_message_text(full_message_id_, std::move(reply_markup_),
                                             std::move(input_message_content_), std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_message_object(full_message_id_));
  }

 public:
  EditMessageTextRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id,
                         tl_object_ptr<td_api::ReplyMarkup> reply_markup,
                         tl_object_ptr<td_api::InputMessageContent> input_message_content)
      : RequestOnceActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , reply_markup_(std::move(reply_markup))
      , input_message_content_(std::move(input_message_content)) {
  }
};

class EditMessageLiveLocationRequest : public RequestOnceActor {
  FullMessageId full_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;
  tl_object_ptr<td_api::location> location_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_message_live_location(full_message_id_, std::move(reply_markup_), std::move(location_),
                                                      std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_message_object(full_message_id_));
  }

 public:
  EditMessageLiveLocationRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id,
                                 tl_object_ptr<td_api::ReplyMarkup> reply_markup,
                                 tl_object_ptr<td_api::location> location)
      : RequestOnceActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , reply_markup_(std::move(reply_markup))
      , location_(std::move(location)) {
  }
};

class EditMessageCaptionRequest : public RequestOnceActor {
  FullMessageId full_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;
  tl_object_ptr<td_api::formattedText> caption_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_message_caption(full_message_id_, std::move(reply_markup_), std::move(caption_),
                                                std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_message_object(full_message_id_));
  }

 public:
  EditMessageCaptionRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id,
                            tl_object_ptr<td_api::ReplyMarkup> reply_markup,
                            tl_object_ptr<td_api::formattedText> caption)
      : RequestOnceActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , reply_markup_(std::move(reply_markup))
      , caption_(std::move(caption)) {
  }
};

class EditMessageReplyMarkupRequest : public RequestOnceActor {
  FullMessageId full_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_message_reply_markup(full_message_id_, std::move(reply_markup_), std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_message_object(full_message_id_));
  }

 public:
  EditMessageReplyMarkupRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id,
                                tl_object_ptr<td_api::ReplyMarkup> reply_markup)
      : RequestOnceActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , reply_markup_(std::move(reply_markup)) {
  }
};

class EditInlineMessageTextRequest : public RequestOnceActor {
  string inline_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;
  tl_object_ptr<td_api::InputMessageContent> input_message_content_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_inline_message_text(inline_message_id_, std::move(reply_markup_),
                                                    std::move(input_message_content_), std::move(promise));
  }

 public:
  EditInlineMessageTextRequest(ActorShared<Td> td, uint64 request_id, string inline_message_id,
                               tl_object_ptr<td_api::ReplyMarkup> reply_markup,
                               tl_object_ptr<td_api::InputMessageContent> input_message_content)
      : RequestOnceActor(std::move(td), request_id)
      , inline_message_id_(std::move(inline_message_id))
      , reply_markup_(std::move(reply_markup))
      , input_message_content_(std::move(input_message_content)) {
  }
};

class EditInlineMessageLiveLocationRequest : public RequestOnceActor {
  string inline_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;
  tl_object_ptr<td_api::location> location_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_inline_message_live_location(inline_message_id_, std::move(reply_markup_),
                                                             std::move(location_), std::move(promise));
  }

 public:
  EditInlineMessageLiveLocationRequest(ActorShared<Td> td, uint64 request_id, string inline_message_id,
                                       tl_object_ptr<td_api::ReplyMarkup> reply_markup,
                                       tl_object_ptr<td_api::location> location)
      : RequestOnceActor(std::move(td), request_id)
      , inline_message_id_(std::move(inline_message_id))
      , reply_markup_(std::move(reply_markup))
      , location_(std::move(location)) {
  }
};

class EditInlineMessageCaptionRequest : public RequestOnceActor {
  string inline_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;
  tl_object_ptr<td_api::formattedText> caption_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_inline_message_caption(inline_message_id_, std::move(reply_markup_),
                                                       std::move(caption_), std::move(promise));
  }

 public:
  EditInlineMessageCaptionRequest(ActorShared<Td> td, uint64 request_id, string inline_message_id,
                                  tl_object_ptr<td_api::ReplyMarkup> reply_markup,
                                  tl_object_ptr<td_api::formattedText> caption)
      : RequestOnceActor(std::move(td), request_id)
      , inline_message_id_(std::move(inline_message_id))
      , reply_markup_(std::move(reply_markup))
      , caption_(std::move(caption)) {
  }
};

class EditInlineMessageReplyMarkupRequest : public RequestOnceActor {
  string inline_message_id_;
  tl_object_ptr<td_api::ReplyMarkup> reply_markup_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->edit_inline_message_reply_markup(inline_message_id_, std::move(reply_markup_),
                                                            std::move(promise));
  }

 public:
  EditInlineMessageReplyMarkupRequest(ActorShared<Td> td, uint64 request_id, string inline_message_id,
                                      tl_object_ptr<td_api::ReplyMarkup> reply_markup)
      : RequestOnceActor(std::move(td), request_id)
      , inline_message_id_(std::move(inline_message_id))
      , reply_markup_(std::move(reply_markup)) {
  }
};

class SetGameScoreRequest : public RequestOnceActor {
  FullMessageId full_message_id_;
  bool edit_message_;
  UserId user_id_;
  int32 score_;
  bool force_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->set_game_score(full_message_id_, edit_message_, user_id_, score_, force_,
                                          std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_message_object(full_message_id_));
  }

 public:
  SetGameScoreRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id, bool edit_message,
                      int32 user_id, int32 score, bool force)
      : RequestOnceActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , edit_message_(edit_message)
      , user_id_(user_id)
      , score_(score)
      , force_(force) {
  }
};

class SetInlineGameScoreRequest : public RequestOnceActor {
  string inline_message_id_;
  bool edit_message_;
  UserId user_id_;
  int32 score_;
  bool force_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->set_inline_game_score(inline_message_id_, edit_message_, user_id_, score_, force_,
                                                 std::move(promise));
  }

 public:
  SetInlineGameScoreRequest(ActorShared<Td> td, uint64 request_id, string inline_message_id, bool edit_message,
                            int32 user_id, int32 score, bool force)
      : RequestOnceActor(std::move(td), request_id)
      , inline_message_id_(std::move(inline_message_id))
      , edit_message_(edit_message)
      , user_id_(user_id)
      , score_(score)
      , force_(force) {
  }
};

class GetGameHighScoresRequest : public RequestOnceActor {
  FullMessageId full_message_id_;
  UserId user_id_;

  int64 random_id_;

  void do_run(Promise<Unit> &&promise) override {
    random_id_ = td->messages_manager_->get_game_high_scores(full_message_id_, user_id_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(random_id_ != 0);
    send_result(td->messages_manager_->get_game_high_scores_object(random_id_));
  }

 public:
  GetGameHighScoresRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id, int32 user_id)
      : RequestOnceActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , user_id_(user_id)
      , random_id_(0) {
  }
};

class GetInlineGameHighScoresRequest : public RequestOnceActor {
  string inline_message_id_;
  UserId user_id_;

  int64 random_id_;

  void do_run(Promise<Unit> &&promise) override {
    random_id_ = td->messages_manager_->get_inline_game_high_scores(inline_message_id_, user_id_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(random_id_ != 0);
    send_result(td->messages_manager_->get_game_high_scores_object(random_id_));
  }

 public:
  GetInlineGameHighScoresRequest(ActorShared<Td> td, uint64 request_id, string inline_message_id, int32 user_id)
      : RequestOnceActor(std::move(td), request_id)
      , inline_message_id_(std::move(inline_message_id))
      , user_id_(user_id)
      , random_id_(0) {
  }
};

class SendChatActionRequest : public RequestOnceActor {
  DialogId dialog_id_;
  tl_object_ptr<td_api::ChatAction> action_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->send_dialog_action(dialog_id_, action_, std::move(promise));
  }

 public:
  SendChatActionRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id,
                        tl_object_ptr<td_api::ChatAction> &&action)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id), action_(std::move(action)) {
  }
};

class GetChatHistoryRequest : public RequestActor<> {
  DialogId dialog_id_;
  MessageId from_message_id_;
  int32 offset_;
  int32 limit_;
  bool only_local_;

  tl_object_ptr<td_api::messages> messages_;

  void do_run(Promise<Unit> &&promise) override {
    messages_ = td->messages_manager_->get_dialog_history(dialog_id_, from_message_id_, offset_, limit_,
                                                          get_tries() - 1, only_local_, std::move(promise));
  }

  void do_send_result() override {
    send_result(std::move(messages_));
  }

 public:
  GetChatHistoryRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 from_message_id, int32 offset,
                        int32 limit, bool only_local)
      : RequestActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , from_message_id_(from_message_id)
      , offset_(offset)
      , limit_(limit)
      , only_local_(only_local) {
    if (!only_local_) {
      set_tries(4);
    }
  }
};

class DeleteChatHistoryRequest : public RequestOnceActor {
  DialogId dialog_id_;
  bool remove_from_chat_list_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->delete_dialog_history(dialog_id_, remove_from_chat_list_, std::move(promise));
  }

 public:
  DeleteChatHistoryRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, bool remove_from_chat_list)
      : RequestOnceActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , remove_from_chat_list_(remove_from_chat_list) {
  }
};

class SearchChatMessagesRequest : public RequestActor<> {
  DialogId dialog_id_;
  string query_;
  UserId sender_user_id_;
  MessageId from_message_id_;
  int32 offset_;
  int32 limit_;
  tl_object_ptr<td_api::SearchMessagesFilter> filter_;
  int64 random_id_;

  std::pair<int32, vector<MessageId>> messages_;

  void do_run(Promise<Unit> &&promise) override {
    messages_ = td->messages_manager_->search_dialog_messages(dialog_id_, query_, sender_user_id_, from_message_id_,
                                                              offset_, limit_, filter_, random_id_, get_tries() == 3,
                                                              std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_messages_object(messages_.first, dialog_id_, messages_.second));
  }

  void do_send_error(Status &&status) override {
    if (status.message() == "SEARCH_QUERY_EMPTY") {
      messages_.first = 0;
      messages_.second.clear();
      return do_send_result();
    }
    send_error(std::move(status));
  }

 public:
  SearchChatMessagesRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, string query, int32 user_id,
                            int64 from_message_id, int32 offset, int32 limit,
                            tl_object_ptr<td_api::SearchMessagesFilter> filter)
      : RequestActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , query_(std::move(query))
      , sender_user_id_(user_id)
      , from_message_id_(from_message_id)
      , offset_(offset)
      , limit_(limit)
      , filter_(std::move(filter))
      , random_id_(0) {
    set_tries(3);
  }
};

class OfflineSearchMessagesRequest : public RequestActor<> {
  DialogId dialog_id_;
  string query_;
  int64 from_search_id_;
  int32 limit_;
  tl_object_ptr<td_api::SearchMessagesFilter> filter_;
  int64 random_id_;

  std::pair<int64, vector<FullMessageId>> messages_;

  void do_run(Promise<Unit> &&promise) override {
    messages_ = td->messages_manager_->offline_search_messages(dialog_id_, query_, from_search_id_, limit_, filter_,
                                                               random_id_, std::move(promise));
  }

  void do_send_result() override {
    vector<tl_object_ptr<td_api::message>> result;
    result.reserve(messages_.second.size());
    for (auto full_message_id : messages_.second) {
      result.push_back(td->messages_manager_->get_message_object(full_message_id));
    }

    send_result(make_tl_object<td_api::foundMessages>(std::move(result), messages_.first));
  }

 public:
  OfflineSearchMessagesRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, string query,
                               int64 from_search_id, int32 limit, tl_object_ptr<td_api::SearchMessagesFilter> filter)
      : RequestActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , query_(std::move(query))
      , from_search_id_(from_search_id)
      , limit_(limit)
      , filter_(std::move(filter))
      , random_id_(0) {
  }
};

class SearchMessagesRequest : public RequestActor<> {
  string query_;
  int32 offset_date_;
  DialogId offset_dialog_id_;
  MessageId offset_message_id_;
  int32 limit_;
  int64 random_id_;

  std::pair<int32, vector<FullMessageId>> messages_;

  void do_run(Promise<Unit> &&promise) override {
    messages_ = td->messages_manager_->search_messages(query_, offset_date_, offset_dialog_id_, offset_message_id_,
                                                       limit_, random_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_messages_object(messages_.first, messages_.second));
  }

  void do_send_error(Status &&status) override {
    if (status.message() == "SEARCH_QUERY_EMPTY") {
      messages_.first = 0;
      messages_.second.clear();
      return do_send_result();
    }
    send_error(std::move(status));
  }

 public:
  SearchMessagesRequest(ActorShared<Td> td, uint64 request_id, string query, int32 offset_date, int64 offset_dialog_id,
                        int64 offset_message_id, int32 limit)
      : RequestActor(std::move(td), request_id)
      , query_(std::move(query))
      , offset_date_(offset_date)
      , offset_dialog_id_(offset_dialog_id)
      , offset_message_id_(offset_message_id)
      , limit_(limit)
      , random_id_(0) {
  }
};

class SearchCallMessagesRequest : public RequestActor<> {
  MessageId from_message_id_;
  int32 limit_;
  bool only_missed_;
  int64 random_id_;

  std::pair<int32, vector<FullMessageId>> messages_;

  void do_run(Promise<Unit> &&promise) override {
    messages_ = td->messages_manager_->search_call_messages(from_message_id_, limit_, only_missed_, random_id_,
                                                            get_tries() == 3, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_messages_object(messages_.first, messages_.second));
  }

 public:
  SearchCallMessagesRequest(ActorShared<Td> td, uint64 request_id, int64 from_message_id, int32 limit, bool only_missed)
      : RequestActor(std::move(td), request_id)
      , from_message_id_(from_message_id)
      , limit_(limit)
      , only_missed_(only_missed)
      , random_id_(0) {
    set_tries(3);
  }
};

class SearchChatRecentLocationMessagesRequest : public RequestActor<> {
  DialogId dialog_id_;
  int32 limit_;
  int64 random_id_;

  std::pair<int32, vector<MessageId>> messages_;

  void do_run(Promise<Unit> &&promise) override {
    messages_ = td->messages_manager_->search_dialog_recent_location_messages(dialog_id_, limit_, random_id_,
                                                                              std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_messages_object(messages_.first, dialog_id_, messages_.second));
  }

 public:
  SearchChatRecentLocationMessagesRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int32 limit)
      : RequestActor(std::move(td), request_id), dialog_id_(dialog_id), limit_(limit), random_id_(0) {
  }
};

class GetActiveLiveLocationMessagesRequest : public RequestActor<> {
  vector<FullMessageId> full_message_ids_;

  void do_run(Promise<Unit> &&promise) override {
    full_message_ids_ = td->messages_manager_->get_active_live_location_messages(std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_messages_object(-1, full_message_ids_));
  }

 public:
  GetActiveLiveLocationMessagesRequest(ActorShared<Td> td, uint64 request_id)
      : RequestActor(std::move(td), request_id) {
  }
};

class GetChatMessageByDateRequest : public RequestOnceActor {
  DialogId dialog_id_;
  int32 date_;

  int64 random_id_;

  void do_run(Promise<Unit> &&promise) override {
    random_id_ = td->messages_manager_->get_dialog_message_by_date(dialog_id_, date_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_dialog_message_by_date_object(random_id_));
  }

 public:
  GetChatMessageByDateRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int32 date)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id), date_(date), random_id_(0) {
  }
};

class DeleteMessagesRequest : public RequestOnceActor {
  DialogId dialog_id_;
  vector<MessageId> message_ids_;
  bool revoke_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->delete_messages(dialog_id_, message_ids_, revoke_, std::move(promise));
  }

 public:
  DeleteMessagesRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, vector<int64> message_ids, bool revoke)
      : RequestOnceActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , message_ids_(MessagesManager::get_message_ids(message_ids))
      , revoke_(revoke) {
  }
};

class DeleteChatMessagesFromUserRequest : public RequestOnceActor {
  DialogId dialog_id_;
  UserId user_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->delete_dialog_messages_from_user(dialog_id_, user_id_, std::move(promise));
  }

 public:
  DeleteChatMessagesFromUserRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int32 user_id)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id), user_id_(user_id) {
  }
};

class ReadAllChatMentionsRequest : public RequestOnceActor {
  DialogId dialog_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->read_all_dialog_mentions(dialog_id_, std::move(promise));
  }

 public:
  ReadAllChatMentionsRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id) {
  }
};

class GetWebPagePreviewRequest : public RequestActor<> {
  string message_text_;

  WebPageId web_page_id_;

  void do_run(Promise<Unit> &&promise) override {
    web_page_id_ = td->web_pages_manager_->get_web_page_preview(message_text_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->web_pages_manager_->get_web_page_object(web_page_id_));
  }

 public:
  GetWebPagePreviewRequest(ActorShared<Td> td, uint64 request_id, string message_text)
      : RequestActor(std::move(td), request_id), message_text_(std::move(message_text)) {
  }
};

class GetWebPageInstantViewRequest : public RequestActor<> {
  string url_;
  bool force_full_;

  WebPageId web_page_id_;

  void do_run(Promise<Unit> &&promise) override {
    web_page_id_ = td->web_pages_manager_->get_web_page_instant_view(url_, force_full_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->web_pages_manager_->get_web_page_instant_view_object(web_page_id_));
  }

 public:
  GetWebPageInstantViewRequest(ActorShared<Td> td, uint64 request_id, string url, bool force_full)
      : RequestActor(std::move(td), request_id), url_(std::move(url)), force_full_(force_full) {
    set_tries(3);
  }
};

class CreateChatRequest : public RequestActor<> {
  DialogId dialog_id_;
  bool force_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->create_dialog(dialog_id_, force_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->messages_manager_->get_chat_object(dialog_id_));
  }

 public:
  CreateChatRequest(ActorShared<Td> td, uint64 request_id, DialogId dialog_id, bool force)
      : RequestActor<>(std::move(td), request_id), dialog_id_(dialog_id), force_(force) {
  }
};

class CreateNewGroupChatRequest : public RequestActor<> {
  vector<UserId> user_ids_;
  string title_;
  int64 random_id_;

  DialogId dialog_id_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_id_ = td->messages_manager_->create_new_group_chat(user_ids_, title_, random_id_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(dialog_id_.is_valid());
    send_result(td->messages_manager_->get_chat_object(dialog_id_));
  }

 public:
  CreateNewGroupChatRequest(ActorShared<Td> td, uint64 request_id, vector<int32> user_ids, string title)
      : RequestActor(std::move(td), request_id), title_(std::move(title)), random_id_(0) {
    for (auto user_id : user_ids) {
      user_ids_.emplace_back(user_id);
    }
  }
};

class CreateNewSecretChatRequest : public RequestActor<SecretChatId> {
  UserId user_id_;
  SecretChatId secret_chat_id_;

  void do_run(Promise<SecretChatId> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(secret_chat_id_));
      return;
    }
    td->messages_manager_->create_new_secret_chat(user_id_, std::move(promise));
  }

  void do_set_result(SecretChatId &&result) override {
    secret_chat_id_ = result;
    LOG(INFO) << "New " << secret_chat_id_ << " created";
  }

  void do_send_result() override {
    CHECK(secret_chat_id_.is_valid());
    // SecretChatActor will send this update by himself.
    // But since the update may still be on its way, we will update essential fields here.
    td->contacts_manager_->on_update_secret_chat(
        secret_chat_id_, 0 /* no access_hash */, user_id_, SecretChatState::Unknown, true /* it is outbound chat */,
        -1 /* unknown ttl */, 0 /* unknown creation date */, "" /* no key_hash */, 0);
    DialogId dialog_id(secret_chat_id_);
    td->messages_manager_->force_create_dialog(dialog_id, "create new secret chat");
    send_result(td->messages_manager_->get_chat_object(dialog_id));
  }

 public:
  CreateNewSecretChatRequest(ActorShared<Td> td, uint64 request_id, int32 user_id)
      : RequestActor(std::move(td), request_id), user_id_(user_id) {
  }
};

class CreateNewSupergroupChatRequest : public RequestActor<> {
  string title_;
  bool is_megagroup_;
  string description_;
  int64 random_id_;

  DialogId dialog_id_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_id_ = td->messages_manager_->create_new_channel_chat(title_, is_megagroup_, description_, random_id_,
                                                                std::move(promise));
  }

  void do_send_result() override {
    CHECK(dialog_id_.is_valid());
    send_result(td->messages_manager_->get_chat_object(dialog_id_));
  }

 public:
  CreateNewSupergroupChatRequest(ActorShared<Td> td, uint64 request_id, string title, bool is_megagroup,
                                 string description)
      : RequestActor(std::move(td), request_id)
      , title_(std::move(title))
      , is_megagroup_(is_megagroup)
      , description_(std::move(description))
      , random_id_(0) {
  }
};

class UpgradeGroupChatToSupergroupChatRequest : public RequestActor<> {
  string title_;
  DialogId dialog_id_;

  DialogId result_dialog_id_;

  void do_run(Promise<Unit> &&promise) override {
    result_dialog_id_ = td->messages_manager_->migrate_dialog_to_megagroup(dialog_id_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(result_dialog_id_.is_valid());
    send_result(td->messages_manager_->get_chat_object(result_dialog_id_));
  }

 public:
  UpgradeGroupChatToSupergroupChatRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id)
      : RequestActor(std::move(td), request_id), dialog_id_(dialog_id) {
  }
};

class SetChatPhotoRequest : public RequestOnceActor {
  DialogId dialog_id_;
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->set_dialog_photo(dialog_id_, input_file_, std::move(promise));
  }

 public:
  SetChatPhotoRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id,
                      tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id), input_file_(std::move(input_file)) {
  }
};

class SetChatTitleRequest : public RequestOnceActor {
  DialogId dialog_id_;
  string title_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->set_dialog_title(dialog_id_, title_, std::move(promise));
  }

 public:
  SetChatTitleRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, string &&title)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id), title_(std::move(title)) {
  }
};

class AddChatMemberRequest : public RequestOnceActor {
  DialogId dialog_id_;
  UserId user_id_;
  int32 forward_limit_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->add_dialog_participant(dialog_id_, user_id_, forward_limit_, std::move(promise));
  }

 public:
  AddChatMemberRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int32 user_id, int32 forward_limit)
      : RequestOnceActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , user_id_(user_id)
      , forward_limit_(forward_limit) {
  }
};

class AddChatMembersRequest : public RequestOnceActor {
  DialogId dialog_id_;
  vector<UserId> user_ids_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->add_dialog_participants(dialog_id_, user_ids_, std::move(promise));
  }

 public:
  AddChatMembersRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, const vector<int32> &user_ids)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id) {
    for (auto &user_id : user_ids) {
      user_ids_.emplace_back(user_id);
    }
  }
};

class SetChatMemberStatusRequest : public RequestOnceActor {
  DialogId dialog_id_;
  UserId user_id_;
  tl_object_ptr<td_api::ChatMemberStatus> status_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->set_dialog_participant_status(dialog_id_, user_id_, status_, std::move(promise));
  }

 public:
  SetChatMemberStatusRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int32 user_id,
                             tl_object_ptr<td_api::ChatMemberStatus> &&status)
      : RequestOnceActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , user_id_(user_id)
      , status_(std::move(status)) {
  }
};

class GetChatMemberRequest : public RequestActor<> {
  DialogId dialog_id_;
  UserId user_id_;
  int64 random_id_;

  DialogParticipant dialog_participant_;

  void do_run(Promise<Unit> &&promise) override {
    dialog_participant_ = td->messages_manager_->get_dialog_participant(dialog_id_, user_id_, random_id_,
                                                                        get_tries() < 3, std::move(promise));
  }

  void do_send_result() override {
    if (!td->contacts_manager_->have_user(user_id_)) {
      return send_error(Status::Error(3, "User not found"));
    }
    send_result(td->contacts_manager_->get_chat_member_object(dialog_participant_));
  }

  void do_send_error(Status &&status) override {
    send_error(std::move(status));
  }

 public:
  GetChatMemberRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int32 user_id)
      : RequestActor(std::move(td), request_id), dialog_id_(dialog_id), user_id_(user_id), random_id_(0) {
    set_tries(3);
  }
};

class SearchChatMembersRequest : public RequestActor<> {
  DialogId dialog_id_;
  string query_;
  int32 limit_;
  int64 random_id_ = 0;

  std::pair<int32, vector<DialogParticipant>> participants_;

  void do_run(Promise<Unit> &&promise) override {
    participants_ = td->messages_manager_->search_dialog_participants(dialog_id_, query_, limit_, random_id_,
                                                                      get_tries() < 3, std::move(promise));
  }

  void do_send_result() override {
    // TODO create function get_chat_members_object
    vector<tl_object_ptr<td_api::chatMember>> result;
    result.reserve(participants_.second.size());
    for (auto participant : participants_.second) {
      result.push_back(td->contacts_manager_->get_chat_member_object(participant));
    }

    send_result(make_tl_object<td_api::chatMembers>(participants_.first, std::move(result)));
  }

 public:
  SearchChatMembersRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, string &&query, int32 limit)
      : RequestActor(std::move(td), request_id), dialog_id_(dialog_id), query_(std::move(query)), limit_(limit) {
    set_tries(3);
  }
};

class GetChatAdministratorsRequest : public RequestActor<> {
  DialogId dialog_id_;

  vector<UserId> user_ids_;

  void do_run(Promise<Unit> &&promise) override {
    user_ids_ = td->messages_manager_->get_dialog_administrators(dialog_id_, get_tries(), std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_users_object(-1, user_ids_));
  }

 public:
  GetChatAdministratorsRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id)
      : RequestActor(std::move(td), request_id), dialog_id_(dialog_id) {
    set_tries(3);
  }
};

class GenerateChatInviteLinkRequest : public RequestOnceActor {
  DialogId dialog_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->export_dialog_invite_link(dialog_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::chatInviteLink>(td->messages_manager_->get_dialog_invite_link(dialog_id_)));
  }

 public:
  GenerateChatInviteLinkRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id) {
  }
};

class CheckChatInviteLinkRequest : public RequestActor<> {
  string invite_link_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->check_dialog_invite_link(invite_link_, std::move(promise));
  }

  void do_send_result() override {
    auto result = td->contacts_manager_->get_chat_invite_link_info_object(invite_link_);
    CHECK(result != nullptr);
    send_result(std::move(result));
  }

 public:
  CheckChatInviteLinkRequest(ActorShared<Td> td, uint64 request_id, string invite_link)
      : RequestActor(std::move(td), request_id), invite_link_(std::move(invite_link)) {
  }
};

class JoinChatByInviteLinkRequest : public RequestOnceActor {
  string invite_link_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->import_dialog_invite_link(invite_link_, std::move(promise));
  }

 public:
  JoinChatByInviteLinkRequest(ActorShared<Td> td, uint64 request_id, string invite_link)
      : RequestOnceActor(std::move(td), request_id), invite_link_(std::move(invite_link)) {
  }
};

class GetChatEventLogRequest : public RequestOnceActor {
  DialogId dialog_id_;
  string query_;
  int64 from_event_id_;
  int32 limit_;
  tl_object_ptr<td_api::chatEventLogFilters> filters_;
  vector<UserId> user_ids_;

  int64 random_id_ = 0;

  void do_run(Promise<Unit> &&promise) override {
    random_id_ = td->messages_manager_->get_dialog_event_log(dialog_id_, query_, from_event_id_, limit_, filters_,
                                                             user_ids_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(random_id_ != 0);
    send_result(td->messages_manager_->get_chat_events_object(random_id_));
  }

 public:
  GetChatEventLogRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, string &&query, int64 from_event_id,
                         int32 limit, tl_object_ptr<td_api::chatEventLogFilters> &&filters, vector<int32> user_ids)
      : RequestOnceActor(std::move(td), request_id)
      , dialog_id_(dialog_id)
      , query_(std::move(query))
      , from_event_id_(from_event_id)
      , limit_(limit)
      , filters_(std::move(filters)) {
    for (auto user_id : user_ids) {
      user_ids_.emplace_back(user_id);
    }
  }
};

class GetBlockedUsersRequest : public RequestOnceActor {
  int32 offset_;
  int32 limit_;

  int64 random_id_;

  void do_run(Promise<Unit> &&promise) override {
    random_id_ = td->contacts_manager_->get_blocked_users(offset_, limit_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_blocked_users_object(random_id_));
  }

 public:
  GetBlockedUsersRequest(ActorShared<Td> td, uint64 request_id, int32 offset, int32 limit)
      : RequestOnceActor(std::move(td), request_id), offset_(offset), limit_(limit), random_id_(0) {
  }
};

class ImportContactsRequest : public RequestActor<> {
  vector<tl_object_ptr<td_api::contact>> contacts_;
  int64 random_id_;

  std::pair<vector<UserId>, vector<int32>> imported_contacts_;

  void do_run(Promise<Unit> &&promise) override {
    imported_contacts_ = td->contacts_manager_->import_contacts(contacts_, random_id_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(imported_contacts_.first.size() == contacts_.size());
    CHECK(imported_contacts_.second.size() == contacts_.size());
    send_result(make_tl_object<td_api::importedContacts>(transform(imported_contacts_.first,
                                                                   [this](UserId user_id) {
                                                                     return td->contacts_manager_->get_user_id_object(
                                                                         user_id, "ImportContactsRequest");
                                                                   }),
                                                         std::move(imported_contacts_.second)));
  }

 public:
  ImportContactsRequest(ActorShared<Td> td, uint64 request_id, vector<tl_object_ptr<td_api::contact>> &&contacts)
      : RequestActor(std::move(td), request_id), contacts_(std::move(contacts)), random_id_(0) {
    set_tries(3);  // load_contacts + import_contacts
  }
};

class SearchContactsRequest : public RequestActor<> {
  string query_;
  int32 limit_;

  std::pair<int32, vector<UserId>> user_ids_;

  void do_run(Promise<Unit> &&promise) override {
    user_ids_ = td->contacts_manager_->search_contacts(query_, limit_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_users_object(user_ids_.first, user_ids_.second));
  }

 public:
  SearchContactsRequest(ActorShared<Td> td, uint64 request_id, string query, int32 limit)
      : RequestActor(std::move(td), request_id), query_(std::move(query)), limit_(limit) {
  }
};

class RemoveContactsRequest : public RequestActor<> {
  vector<UserId> user_ids_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->remove_contacts(user_ids_, std::move(promise));
  }

 public:
  RemoveContactsRequest(ActorShared<Td> td, uint64 request_id, vector<int32> &&user_ids)
      : RequestActor(std::move(td), request_id) {
    for (auto user_id : user_ids) {
      user_ids_.emplace_back(user_id);
    }
    set_tries(3);  // load_contacts + delete_contacts
  }
};

class GetImportedContactCountRequest : public RequestActor<> {
  int32 imported_contact_count_ = 0;

  void do_run(Promise<Unit> &&promise) override {
    imported_contact_count_ = td->contacts_manager_->get_imported_contact_count(std::move(promise));
  }

  void do_send_result() override {
    send_result(td_api::make_object<td_api::count>(imported_contact_count_));
  }

 public:
  GetImportedContactCountRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class ChangeImportedContactsRequest : public RequestActor<> {
  vector<tl_object_ptr<td_api::contact>> contacts_;
  size_t contacts_size_;
  int64 random_id_;

  std::pair<vector<UserId>, vector<int32>> imported_contacts_;

  void do_run(Promise<Unit> &&promise) override {
    imported_contacts_ =
        td->contacts_manager_->change_imported_contacts(std::move(contacts_), random_id_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(imported_contacts_.first.size() == contacts_size_);
    CHECK(imported_contacts_.second.size() == contacts_size_);
    send_result(make_tl_object<td_api::importedContacts>(transform(imported_contacts_.first,
                                                                   [this](UserId user_id) {
                                                                     return td->contacts_manager_->get_user_id_object(
                                                                         user_id, "ChangeImportedContactsRequest");
                                                                   }),
                                                         std::move(imported_contacts_.second)));
  }

 public:
  ChangeImportedContactsRequest(ActorShared<Td> td, uint64 request_id,
                                vector<tl_object_ptr<td_api::contact>> &&contacts)
      : RequestActor(std::move(td), request_id)
      , contacts_(std::move(contacts))
      , contacts_size_(contacts_.size())
      , random_id_(0) {
    set_tries(4);  // load_contacts + load_local_contacts + (import_contacts + delete_contacts)
  }
};

class ClearImportedContactsRequest : public RequestActor<> {
  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->clear_imported_contacts(std::move(promise));
  }

 public:
  ClearImportedContactsRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
    set_tries(3);  // load_contacts + clear
  }
};

class GetRecentInlineBotsRequest : public RequestActor<> {
  vector<UserId> user_ids_;

  void do_run(Promise<Unit> &&promise) override {
    user_ids_ = td->inline_queries_manager_->get_recent_inline_bots(std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_users_object(-1, user_ids_));
  }

 public:
  GetRecentInlineBotsRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class SetNameRequest : public RequestOnceActor {
  string first_name_;
  string last_name_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_name(first_name_, last_name_, std::move(promise));
  }

 public:
  SetNameRequest(ActorShared<Td> td, uint64 request_id, string first_name, string last_name)
      : RequestOnceActor(std::move(td), request_id)
      , first_name_(std::move(first_name))
      , last_name_(std::move(last_name)) {
  }
};

class SetBioRequest : public RequestOnceActor {
  string bio_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_bio(bio_, std::move(promise));
  }

 public:
  SetBioRequest(ActorShared<Td> td, uint64 request_id, string bio)
      : RequestOnceActor(std::move(td), request_id), bio_(std::move(bio)) {
  }
};

class SetUsernameRequest : public RequestOnceActor {
  string username_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_username(username_, std::move(promise));
  }

 public:
  SetUsernameRequest(ActorShared<Td> td, uint64 request_id, string username)
      : RequestOnceActor(std::move(td), request_id), username_(std::move(username)) {
  }
};

class SetProfilePhotoRequest : public RequestOnceActor {
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_profile_photo(input_file_, std::move(promise));
  }

 public:
  SetProfilePhotoRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestOnceActor(std::move(td), request_id), input_file_(std::move(input_file)) {
  }
};

class DeleteProfilePhotoRequest : public RequestOnceActor {
  int64 profile_photo_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->delete_profile_photo(profile_photo_id_, std::move(promise));
  }

 public:
  DeleteProfilePhotoRequest(ActorShared<Td> td, uint64 request_id, int64 profile_photo_id)
      : RequestOnceActor(std::move(td), request_id), profile_photo_id_(profile_photo_id) {
  }
};

class ToggleGroupAdministratorsRequest : public RequestOnceActor {
  ChatId chat_id_;
  bool everyone_is_administrator_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->toggle_chat_administrators(chat_id_, everyone_is_administrator_, std::move(promise));
  }

 public:
  ToggleGroupAdministratorsRequest(ActorShared<Td> td, uint64 request_id, int32 chat_id, bool everyone_is_administrator)
      : RequestOnceActor(std::move(td), request_id)
      , chat_id_(chat_id)
      , everyone_is_administrator_(everyone_is_administrator) {
  }
};

class SetSupergroupUsernameRequest : public RequestOnceActor {
  ChannelId channel_id_;
  string username_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_channel_username(channel_id_, username_, std::move(promise));
  }

 public:
  SetSupergroupUsernameRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id, string username)
      : RequestOnceActor(std::move(td), request_id), channel_id_(channel_id), username_(std::move(username)) {
  }
};

class SetSupergroupStickerSetRequest : public RequestOnceActor {
  ChannelId channel_id_;
  int64 sticker_set_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_channel_sticker_set(channel_id_, sticker_set_id_, std::move(promise));
  }

 public:
  SetSupergroupStickerSetRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id, int64 sticker_set_id)
      : RequestOnceActor(std::move(td), request_id), channel_id_(channel_id), sticker_set_id_(sticker_set_id) {
  }
};

class ToggleSupergroupInvitesRequest : public RequestOnceActor {
  ChannelId channel_id_;
  bool anyone_can_invite_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->toggle_channel_invites(channel_id_, anyone_can_invite_, std::move(promise));
  }

 public:
  ToggleSupergroupInvitesRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id, bool anyone_can_invite)
      : RequestOnceActor(std::move(td), request_id), channel_id_(channel_id), anyone_can_invite_(anyone_can_invite) {
  }
};

class ToggleSupergroupSignMessagesRequest : public RequestOnceActor {
  ChannelId channel_id_;
  bool sign_messages_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->toggle_channel_sign_messages(channel_id_, sign_messages_, std::move(promise));
  }

 public:
  ToggleSupergroupSignMessagesRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id, bool sign_messages)
      : RequestOnceActor(std::move(td), request_id), channel_id_(channel_id), sign_messages_(sign_messages) {
  }
};

class ToggleSupergroupIsAllHistoryAvailableRequest : public RequestOnceActor {
  ChannelId channel_id_;
  bool is_all_history_available_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->toggle_channel_is_all_history_available(channel_id_, is_all_history_available_,
                                                                   std::move(promise));
  }

 public:
  ToggleSupergroupIsAllHistoryAvailableRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id,
                                               bool is_all_history_available)
      : RequestOnceActor(std::move(td), request_id)
      , channel_id_(channel_id)
      , is_all_history_available_(is_all_history_available) {
  }
};

class SetSupergroupDescriptionRequest : public RequestOnceActor {
  ChannelId channel_id_;
  string description_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->set_channel_description(channel_id_, description_, std::move(promise));
  }

 public:
  SetSupergroupDescriptionRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id, string description)
      : RequestOnceActor(std::move(td), request_id), channel_id_(channel_id), description_(std::move(description)) {
  }
};

class PinSupergroupMessageRequest : public RequestOnceActor {
  ChannelId channel_id_;
  MessageId message_id_;
  bool disable_notification_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->pin_channel_message(channel_id_, message_id_, disable_notification_, std::move(promise));
  }

 public:
  PinSupergroupMessageRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id, int64 message_id,
                              bool disable_notification)
      : RequestOnceActor(std::move(td), request_id)
      , channel_id_(channel_id)
      , message_id_(message_id)
      , disable_notification_(disable_notification) {
  }
};

class UnpinSupergroupMessageRequest : public RequestOnceActor {
  ChannelId channel_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->unpin_channel_message(channel_id_, std::move(promise));
  }

 public:
  UnpinSupergroupMessageRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id)
      : RequestOnceActor(std::move(td), request_id), channel_id_(channel_id) {
  }
};

class ReportSupergroupSpamRequest : public RequestOnceActor {
  ChannelId channel_id_;
  UserId user_id_;
  vector<MessageId> message_ids_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->report_channel_spam(channel_id_, user_id_, message_ids_, std::move(promise));
  }

 public:
  ReportSupergroupSpamRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id, int32 user_id,
                              const vector<int64> &message_ids)
      : RequestOnceActor(std::move(td), request_id)
      , channel_id_(channel_id)
      , user_id_(user_id)
      , message_ids_(MessagesManager::get_message_ids(message_ids)) {
  }
};

class GetSupergroupMembersRequest : public RequestActor<> {
  ChannelId channel_id_;
  tl_object_ptr<td_api::SupergroupMembersFilter> filter_;
  int32 offset_;
  int32 limit_;
  int64 random_id_ = 0;

  std::pair<int32, vector<DialogParticipant>> participants_;

  void do_run(Promise<Unit> &&promise) override {
    participants_ = td->contacts_manager_->get_channel_participants(channel_id_, filter_, offset_, limit_, random_id_,
                                                                    get_tries() < 3, std::move(promise));
  }

  void do_send_result() override {
    // TODO create function get_chat_members_object
    vector<tl_object_ptr<td_api::chatMember>> result;
    result.reserve(participants_.second.size());
    for (auto participant : participants_.second) {
      result.push_back(td->contacts_manager_->get_chat_member_object(participant));
    }

    send_result(make_tl_object<td_api::chatMembers>(participants_.first, std::move(result)));
  }

 public:
  GetSupergroupMembersRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id,
                              tl_object_ptr<td_api::SupergroupMembersFilter> &&filter, int32 offset, int32 limit)
      : RequestActor(std::move(td), request_id)
      , channel_id_(channel_id)
      , filter_(std::move(filter))
      , offset_(offset)
      , limit_(limit) {
    set_tries(3);
  }
};

class DeleteSupergroupRequest : public RequestOnceActor {
  ChannelId channel_id_;

  void do_run(Promise<Unit> &&promise) override {
    td->contacts_manager_->delete_channel(channel_id_, std::move(promise));
  }

 public:
  DeleteSupergroupRequest(ActorShared<Td> td, uint64 request_id, int32 channel_id)
      : RequestOnceActor(std::move(td), request_id), channel_id_(channel_id) {
  }
};

class GetUserProfilePhotosRequest : public RequestActor<> {
  UserId user_id_;
  int32 offset_;
  int32 limit_;
  std::pair<int32, vector<const Photo *>> photos;

  void do_run(Promise<Unit> &&promise) override {
    photos = td->contacts_manager_->get_user_profile_photos(user_id_, offset_, limit_, std::move(promise));
  }

  void do_send_result() override {
    // TODO create function get_user_profile_photos_object
    vector<tl_object_ptr<td_api::photo>> result;
    result.reserve(photos.second.size());
    for (auto photo : photos.second) {
      result.push_back(get_photo_object(td->file_manager_.get(), photo));
    }

    send_result(make_tl_object<td_api::userProfilePhotos>(photos.first, std::move(result)));
  }

 public:
  GetUserProfilePhotosRequest(ActorShared<Td> td, uint64 request_id, int32 user_id, int32 offset, int32 limit)
      : RequestActor(std::move(td), request_id), user_id_(user_id), offset_(offset), limit_(limit) {
  }
};

class GetNotificationSettingsRequest : public RequestActor<> {
  NotificationSettingsScope scope_;

  const NotificationSettings *notification_settings_ = nullptr;

  void do_run(Promise<Unit> &&promise) override {
    notification_settings_ = td->messages_manager_->get_notification_settings(scope_, std::move(promise));
  }

  void do_send_result() override {
    CHECK(notification_settings_ != nullptr);
    send_result(td->messages_manager_->get_notification_settings_object(notification_settings_));
  }

 public:
  GetNotificationSettingsRequest(ActorShared<Td> td, uint64 request_id, NotificationSettingsScope scope)
      : RequestActor(std::move(td), request_id), scope_(scope) {
  }
};

class GetChatReportSpamStateRequest : public RequestActor<> {
  DialogId dialog_id_;

  bool can_report_spam_ = false;

  void do_run(Promise<Unit> &&promise) override {
    can_report_spam_ = td->messages_manager_->get_dialog_report_spam_state(dialog_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::chatReportSpamState>(can_report_spam_));
  }

 public:
  GetChatReportSpamStateRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id)
      : RequestActor(std::move(td), request_id), dialog_id_(dialog_id) {
  }
};

class ChangeChatReportSpamStateRequest : public RequestOnceActor {
  DialogId dialog_id_;
  bool is_spam_dialog_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->change_dialog_report_spam_state(dialog_id_, is_spam_dialog_, std::move(promise));
  }

 public:
  ChangeChatReportSpamStateRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, bool is_spam_dialog)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id), is_spam_dialog_(is_spam_dialog) {
  }
};

class ReportChatRequest : public RequestOnceActor {
  DialogId dialog_id_;
  tl_object_ptr<td_api::ChatReportReason> reason_;

  void do_run(Promise<Unit> &&promise) override {
    td->messages_manager_->report_dialog(dialog_id_, reason_, std::move(promise));
  }

 public:
  ReportChatRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id,
                    tl_object_ptr<td_api::ChatReportReason> reason)
      : RequestOnceActor(std::move(td), request_id), dialog_id_(dialog_id), reason_(std::move(reason)) {
  }
};

class GetStickersRequest : public RequestActor<> {
  string emoji_;
  int32 limit_;

  vector<FileId> sticker_ids_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_ids_ = td->stickers_manager_->get_stickers(emoji_, limit_, get_tries() < 2, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_stickers_object(sticker_ids_));
  }

 public:
  GetStickersRequest(ActorShared<Td> td, uint64 request_id, string &&emoji, int32 limit)
      : RequestActor(std::move(td), request_id), emoji_(std::move(emoji)), limit_(limit) {
    set_tries(5);
  }
};

class GetInstalledStickerSetsRequest : public RequestActor<> {
  bool is_masks_;

  vector<int64> sticker_set_ids_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_set_ids_ = td->stickers_manager_->get_installed_sticker_sets(is_masks_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_sticker_sets_object(-1, sticker_set_ids_, 1));
  }

 public:
  GetInstalledStickerSetsRequest(ActorShared<Td> td, uint64 request_id, bool is_masks)
      : RequestActor(std::move(td), request_id), is_masks_(is_masks) {
  }
};

class GetArchivedStickerSetsRequest : public RequestActor<> {
  bool is_masks_;
  int64 offset_sticker_set_id_;
  int32 limit_;

  int32 total_count_;
  vector<int64> sticker_set_ids_;

  void do_run(Promise<Unit> &&promise) override {
    std::tie(total_count_, sticker_set_ids_) = td->stickers_manager_->get_archived_sticker_sets(
        is_masks_, offset_sticker_set_id_, limit_, get_tries() < 2, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_sticker_sets_object(total_count_, sticker_set_ids_, 1));
  }

 public:
  GetArchivedStickerSetsRequest(ActorShared<Td> td, uint64 request_id, bool is_masks, int64 offset_sticker_set_id,
                                int32 limit)
      : RequestActor(std::move(td), request_id)
      , is_masks_(is_masks)
      , offset_sticker_set_id_(offset_sticker_set_id)
      , limit_(limit) {
  }
};

class GetTrendingStickerSetsRequest : public RequestActor<> {
  vector<int64> sticker_set_ids_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_set_ids_ = td->stickers_manager_->get_featured_sticker_sets(std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_sticker_sets_object(-1, sticker_set_ids_, 5));
  }

 public:
  GetTrendingStickerSetsRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class GetAttachedStickerSetsRequest : public RequestActor<> {
  FileId file_id_;

  vector<int64> sticker_set_ids_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_set_ids_ = td->stickers_manager_->get_attached_sticker_sets(file_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_sticker_sets_object(-1, sticker_set_ids_, 5));
  }

 public:
  GetAttachedStickerSetsRequest(ActorShared<Td> td, uint64 request_id, int32 file_id)
      : RequestActor(std::move(td), request_id), file_id_(file_id) {
  }
};

class GetStickerSetRequest : public RequestActor<> {
  int64 set_id_;

  int64 sticker_set_id_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_set_id_ = td->stickers_manager_->get_sticker_set(set_id_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_sticker_set_object(sticker_set_id_));
  }

 public:
  GetStickerSetRequest(ActorShared<Td> td, uint64 request_id, int64 set_id)
      : RequestActor(std::move(td), request_id), set_id_(set_id) {
    set_tries(3);
  }
};

class SearchStickerSetRequest : public RequestActor<> {
  string name_;

  int64 sticker_set_id_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_set_id_ = td->stickers_manager_->search_sticker_set(name_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_sticker_set_object(sticker_set_id_));
  }

 public:
  SearchStickerSetRequest(ActorShared<Td> td, uint64 request_id, string &&name)
      : RequestActor(std::move(td), request_id), name_(std::move(name)) {
    set_tries(3);
  }
};

class ChangeStickerSetRequest : public RequestOnceActor {
  int64 set_id_;
  bool is_installed_;
  bool is_archived_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->change_sticker_set(set_id_, is_installed_, is_archived_, std::move(promise));
  }

 public:
  ChangeStickerSetRequest(ActorShared<Td> td, uint64 request_id, int64 set_id, bool is_installed, bool is_archived)
      : RequestOnceActor(std::move(td), request_id)
      , set_id_(set_id)
      , is_installed_(is_installed)
      , is_archived_(is_archived) {
    set_tries(3);
  }
};

class ReorderInstalledStickerSetsRequest : public RequestOnceActor {
  bool is_masks_;
  vector<int64> sticker_set_ids_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->reorder_installed_sticker_sets(is_masks_, sticker_set_ids_, std::move(promise));
  }

 public:
  ReorderInstalledStickerSetsRequest(ActorShared<Td> td, uint64 request_id, bool is_masks,
                                     vector<int64> &&sticker_set_ids)
      : RequestOnceActor(std::move(td), request_id), is_masks_(is_masks), sticker_set_ids_(std::move(sticker_set_ids)) {
  }
};

class UploadStickerFileRequest : public RequestOnceActor {
  UserId user_id_;
  tl_object_ptr<td_api::InputFile> sticker_;

  FileId file_id;

  void do_run(Promise<Unit> &&promise) override {
    file_id = td->stickers_manager_->upload_sticker_file(user_id_, sticker_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->file_manager_->get_file_object(file_id));
  }

 public:
  UploadStickerFileRequest(ActorShared<Td> td, uint64 request_id, int32 user_id,
                           tl_object_ptr<td_api::InputFile> &&sticker)
      : RequestOnceActor(std::move(td), request_id), user_id_(user_id), sticker_(std::move(sticker)) {
  }
};

class CreateNewStickerSetRequest : public RequestOnceActor {
  UserId user_id_;
  string title_;
  string name_;
  bool is_masks_;
  vector<tl_object_ptr<td_api::inputSticker>> stickers_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->create_new_sticker_set(user_id_, title_, name_, is_masks_, std::move(stickers_),
                                                  std::move(promise));
  }

  void do_send_result() override {
    auto set_id = td->stickers_manager_->search_sticker_set(name_, Auto());
    if (set_id == 0) {
      return send_error(Status::Error(500, "Created sticker set not found"));
    }
    send_result(td->stickers_manager_->get_sticker_set_object(set_id));
  }

 public:
  CreateNewStickerSetRequest(ActorShared<Td> td, uint64 request_id, int32 user_id, string &&title, string &&name,
                             bool is_masks, vector<tl_object_ptr<td_api::inputSticker>> &&stickers)
      : RequestOnceActor(std::move(td), request_id)
      , user_id_(user_id)
      , title_(std::move(title))
      , name_(std::move(name))
      , is_masks_(is_masks)
      , stickers_(std::move(stickers)) {
  }
};

class AddStickerToSetRequest : public RequestOnceActor {
  UserId user_id_;
  string name_;
  tl_object_ptr<td_api::inputSticker> sticker_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->add_sticker_to_set(user_id_, name_, std::move(sticker_), std::move(promise));
  }

  void do_send_result() override {
    auto set_id = td->stickers_manager_->search_sticker_set(name_, Auto());
    if (set_id == 0) {
      return send_error(Status::Error(500, "Sticker set not found"));
    }
    send_result(td->stickers_manager_->get_sticker_set_object(set_id));
  }

 public:
  AddStickerToSetRequest(ActorShared<Td> td, uint64 request_id, int32 user_id, string &&name,
                         tl_object_ptr<td_api::inputSticker> &&sticker)
      : RequestOnceActor(std::move(td), request_id)
      , user_id_(user_id)
      , name_(std::move(name))
      , sticker_(std::move(sticker)) {
  }
};

class SetStickerPositionInSetRequest : public RequestOnceActor {
  tl_object_ptr<td_api::InputFile> sticker_;
  int32 position_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->set_sticker_position_in_set(sticker_, position_, std::move(promise));
  }

 public:
  SetStickerPositionInSetRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&sticker,
                                 int32 position)
      : RequestOnceActor(std::move(td), request_id), sticker_(std::move(sticker)), position_(position) {
  }
};

class RemoveStickerFromSetRequest : public RequestOnceActor {
  tl_object_ptr<td_api::InputFile> sticker_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->remove_sticker_from_set(sticker_, std::move(promise));
  }

 public:
  RemoveStickerFromSetRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&sticker)
      : RequestOnceActor(std::move(td), request_id), sticker_(std::move(sticker)) {
  }
};

class GetRecentStickersRequest : public RequestActor<> {
  bool is_attached_;

  vector<FileId> sticker_ids_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_ids_ = td->stickers_manager_->get_recent_stickers(is_attached_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_stickers_object(sticker_ids_));
  }

 public:
  GetRecentStickersRequest(ActorShared<Td> td, uint64 request_id, bool is_attached)
      : RequestActor(std::move(td), request_id), is_attached_(is_attached) {
  }
};

class AddRecentStickerRequest : public RequestActor<> {
  bool is_attached_;
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->add_recent_sticker(is_attached_, input_file_, std::move(promise));
  }

 public:
  AddRecentStickerRequest(ActorShared<Td> td, uint64 request_id, bool is_attached,
                          tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestActor(std::move(td), request_id), is_attached_(is_attached), input_file_(std::move(input_file)) {
    set_tries(3);
  }
};

class RemoveRecentStickerRequest : public RequestActor<> {
  bool is_attached_;
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->remove_recent_sticker(is_attached_, input_file_, std::move(promise));
  }

 public:
  RemoveRecentStickerRequest(ActorShared<Td> td, uint64 request_id, bool is_attached,
                             tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestActor(std::move(td), request_id), is_attached_(is_attached), input_file_(std::move(input_file)) {
    set_tries(3);
  }
};

class ClearRecentStickersRequest : public RequestActor<> {
  bool is_attached_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->clear_recent_stickers(is_attached_, std::move(promise));
  }

 public:
  ClearRecentStickersRequest(ActorShared<Td> td, uint64 request_id, bool is_attached)
      : RequestActor(std::move(td), request_id), is_attached_(is_attached) {
    set_tries(3);
  }
};

class GetFavoriteStickersRequest : public RequestActor<> {
  vector<FileId> sticker_ids_;

  void do_run(Promise<Unit> &&promise) override {
    sticker_ids_ = td->stickers_manager_->get_favorite_stickers(std::move(promise));
  }

  void do_send_result() override {
    send_result(td->stickers_manager_->get_stickers_object(sticker_ids_));
  }

 public:
  GetFavoriteStickersRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class AddFavoriteStickerRequest : public RequestOnceActor {
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->add_favorite_sticker(input_file_, std::move(promise));
  }

 public:
  AddFavoriteStickerRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestOnceActor(std::move(td), request_id), input_file_(std::move(input_file)) {
    set_tries(3);
  }
};

class RemoveFavoriteStickerRequest : public RequestOnceActor {
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->stickers_manager_->remove_favorite_sticker(input_file_, std::move(promise));
  }

 public:
  RemoveFavoriteStickerRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestOnceActor(std::move(td), request_id), input_file_(std::move(input_file)) {
    set_tries(3);
  }
};

class GetStickerEmojisRequest : public RequestActor<> {
  tl_object_ptr<td_api::InputFile> input_file_;

  vector<string> emojis_;

  void do_run(Promise<Unit> &&promise) override {
    emojis_ = td->stickers_manager_->get_sticker_emojis(input_file_, std::move(promise));
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::stickerEmojis>(std::move(emojis_)));
  }

 public:
  GetStickerEmojisRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestActor(std::move(td), request_id), input_file_(std::move(input_file)) {
    set_tries(3);
  }
};

class GetSavedAnimationsRequest : public RequestActor<> {
  vector<FileId> animation_ids_;

  void do_run(Promise<Unit> &&promise) override {
    animation_ids_ = td->animations_manager_->get_saved_animations(std::move(promise));
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::animations>(transform(std::move(animation_ids_), [td = td](FileId animation_id) {
      return td->animations_manager_->get_animation_object(animation_id, "GetSavedAnimationsRequest");
    })));
  }

 public:
  GetSavedAnimationsRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class AddSavedAnimationRequest : public RequestOnceActor {
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->animations_manager_->add_saved_animation(input_file_, std::move(promise));
  }

 public:
  AddSavedAnimationRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestOnceActor(std::move(td), request_id), input_file_(std::move(input_file)) {
    set_tries(3);
  }
};

class RemoveSavedAnimationRequest : public RequestOnceActor {
  tl_object_ptr<td_api::InputFile> input_file_;

  void do_run(Promise<Unit> &&promise) override {
    td->animations_manager_->remove_saved_animation(input_file_, std::move(promise));
  }

 public:
  RemoveSavedAnimationRequest(ActorShared<Td> td, uint64 request_id, tl_object_ptr<td_api::InputFile> &&input_file)
      : RequestOnceActor(std::move(td), request_id), input_file_(std::move(input_file)) {
    set_tries(3);
  }
};

class GetInlineQueryResultsRequest : public RequestOnceActor {
  UserId bot_user_id_;
  DialogId dialog_id_;
  Location user_location_;
  string query_;
  string offset_;

  uint64 query_hash_;

  void do_run(Promise<Unit> &&promise) override {
    query_hash_ = td->inline_queries_manager_->send_inline_query(bot_user_id_, dialog_id_, user_location_, query_,
                                                                 offset_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->inline_queries_manager_->get_inline_query_results_object(query_hash_));
  }

 public:
  GetInlineQueryResultsRequest(ActorShared<Td> td, uint64 request_id, int32 bot_user_id, int64 dialog_id,
                               const tl_object_ptr<td_api::location> &user_location, string query, string offset)
      : RequestOnceActor(std::move(td), request_id)
      , bot_user_id_(bot_user_id)
      , dialog_id_(dialog_id)
      , user_location_(user_location)
      , query_(std::move(query))
      , offset_(std::move(offset))
      , query_hash_(0) {
  }
};

class AnswerInlineQueryRequest : public RequestOnceActor {
  int64 inline_query_id_;
  bool is_personal_;
  vector<tl_object_ptr<td_api::InputInlineQueryResult>> results_;
  int32 cache_time_;
  string next_offset_;
  string switch_pm_text_;
  string switch_pm_parameter_;

  void do_run(Promise<Unit> &&promise) override {
    td->inline_queries_manager_->answer_inline_query(inline_query_id_, is_personal_, std::move(results_), cache_time_,
                                                     next_offset_, switch_pm_text_, switch_pm_parameter_,
                                                     std::move(promise));
  }

 public:
  AnswerInlineQueryRequest(ActorShared<Td> td, uint64 request_id, int64 inline_query_id, bool is_personal,
                           vector<tl_object_ptr<td_api::InputInlineQueryResult>> &&results, int32 cache_time,
                           string next_offset, string switch_pm_text, string switch_pm_parameter)
      : RequestOnceActor(std::move(td), request_id)
      , inline_query_id_(inline_query_id)
      , is_personal_(is_personal)
      , results_(std::move(results))
      , cache_time_(cache_time)
      , next_offset_(std::move(next_offset))
      , switch_pm_text_(std::move(switch_pm_text))
      , switch_pm_parameter_(std::move(switch_pm_parameter)) {
  }
};

class GetCallbackQueryAnswerRequest : public RequestOnceActor {
  FullMessageId full_message_id_;
  tl_object_ptr<td_api::CallbackQueryPayload> payload_;

  int64 result_id_;

  void do_run(Promise<Unit> &&promise) override {
    result_id_ = td->callback_queries_manager_->send_callback_query(full_message_id_, payload_, std::move(promise));
  }

  void do_send_result() override {
    send_result(td->callback_queries_manager_->get_callback_query_answer_object(result_id_));
  }

  void do_send_error(Status &&status) override {
    if (status.code() == 502 && td->messages_manager_->is_message_edited_recently(full_message_id_, 31)) {
      return send_result(make_tl_object<td_api::callbackQueryAnswer>());
    }
    send_error(std::move(status));
  }

 public:
  GetCallbackQueryAnswerRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id,
                                tl_object_ptr<td_api::CallbackQueryPayload> payload)
      : RequestOnceActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , payload_(std::move(payload))
      , result_id_(0) {
  }
};

class AnswerCallbackQueryRequest : public RequestOnceActor {
  int64 callback_query_id_;
  string text_;
  bool show_alert_;
  string url_;
  int32 cache_time_;

  void do_run(Promise<Unit> &&promise) override {
    td->callback_queries_manager_->answer_callback_query(callback_query_id_, text_, show_alert_, url_, cache_time_,
                                                         std::move(promise));
  }

 public:
  AnswerCallbackQueryRequest(ActorShared<Td> td, uint64 request_id, int64 callback_query_id, string text,
                             bool show_alert, string url, int32 cache_time)
      : RequestOnceActor(std::move(td), request_id)
      , callback_query_id_(callback_query_id)
      , text_(std::move(text))
      , show_alert_(show_alert)
      , url_(std::move(url))
      , cache_time_(cache_time) {
  }
};

class AnswerShippingQueryRequest : public RequestOnceActor {
  int64 shipping_query_id_;
  vector<tl_object_ptr<td_api::shippingOption>> shipping_options_;
  string error_message_;

  void do_run(Promise<Unit> &&promise) override {
    answer_shipping_query(shipping_query_id_, std::move(shipping_options_), error_message_, std::move(promise));
  }

 public:
  AnswerShippingQueryRequest(ActorShared<Td> td, uint64 request_id, int64 shipping_query_id,
                             vector<tl_object_ptr<td_api::shippingOption>> shipping_options, string error_message)
      : RequestOnceActor(std::move(td), request_id)
      , shipping_query_id_(shipping_query_id)
      , shipping_options_(std::move(shipping_options))
      , error_message_(std::move(error_message)) {
  }
};

class AnswerPreCheckoutQueryRequest : public RequestOnceActor {
  int64 pre_checkout_query_id_;
  string error_message_;

  void do_run(Promise<Unit> &&promise) override {
    answer_pre_checkout_query(pre_checkout_query_id_, error_message_, std::move(promise));
  }

 public:
  AnswerPreCheckoutQueryRequest(ActorShared<Td> td, uint64 request_id, int64 pre_checkout_query_id,
                                string error_message)
      : RequestOnceActor(std::move(td), request_id)
      , pre_checkout_query_id_(pre_checkout_query_id)
      , error_message_(std::move(error_message)) {
  }
};

class GetPaymentFormRequest : public RequestActor<tl_object_ptr<td_api::paymentForm>> {
  FullMessageId full_message_id_;

  tl_object_ptr<td_api::paymentForm> payment_form_;

  void do_run(Promise<tl_object_ptr<td_api::paymentForm>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(payment_form_));
      return;
    }

    td->messages_manager_->get_payment_form(full_message_id_, std::move(promise));
  }

  void do_set_result(tl_object_ptr<td_api::paymentForm> &&result) override {
    payment_form_ = std::move(result);
  }

  void do_send_result() override {
    CHECK(payment_form_ != nullptr);
    send_result(std::move(payment_form_));
  }

 public:
  GetPaymentFormRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id)
      : RequestActor(std::move(td), request_id), full_message_id_(DialogId(dialog_id), MessageId(message_id)) {
  }
};

class ValidateOrderInfoRequest : public RequestActor<tl_object_ptr<td_api::validatedOrderInfo>> {
  FullMessageId full_message_id_;
  tl_object_ptr<td_api::orderInfo> order_info_;
  bool allow_save_;

  tl_object_ptr<td_api::validatedOrderInfo> validated_order_info_;

  void do_run(Promise<tl_object_ptr<td_api::validatedOrderInfo>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(validated_order_info_));
      return;
    }

    td->messages_manager_->validate_order_info(full_message_id_, std::move(order_info_), allow_save_,
                                               std::move(promise));
  }

  void do_set_result(tl_object_ptr<td_api::validatedOrderInfo> &&result) override {
    validated_order_info_ = std::move(result);
  }

  void do_send_result() override {
    CHECK(validated_order_info_ != nullptr);
    send_result(std::move(validated_order_info_));
  }

 public:
  ValidateOrderInfoRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id,
                           tl_object_ptr<td_api::orderInfo> order_info, bool allow_save)
      : RequestActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , order_info_(std::move(order_info))
      , allow_save_(allow_save) {
  }
};

class SendPaymentFormRequest : public RequestActor<tl_object_ptr<td_api::paymentResult>> {
  FullMessageId full_message_id_;
  string order_info_id_;
  string shipping_option_id_;
  tl_object_ptr<td_api::InputCredentials> credentials_;

  tl_object_ptr<td_api::paymentResult> payment_result_;

  void do_run(Promise<tl_object_ptr<td_api::paymentResult>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(payment_result_));
      return;
    }

    td->messages_manager_->send_payment_form(full_message_id_, order_info_id_, shipping_option_id_, credentials_,
                                             std::move(promise));
  }

  void do_set_result(tl_object_ptr<td_api::paymentResult> &&result) override {
    payment_result_ = std::move(result);
  }

  void do_send_result() override {
    CHECK(payment_result_ != nullptr);
    send_result(std::move(payment_result_));
  }

 public:
  SendPaymentFormRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id, string order_info_id,
                         string shipping_option_id, tl_object_ptr<td_api::InputCredentials> credentials)
      : RequestActor(std::move(td), request_id)
      , full_message_id_(DialogId(dialog_id), MessageId(message_id))
      , order_info_id_(std::move(order_info_id))
      , shipping_option_id_(std::move(shipping_option_id))
      , credentials_(std::move(credentials)) {
  }
};

class GetPaymentReceiptRequest : public RequestActor<tl_object_ptr<td_api::paymentReceipt>> {
  FullMessageId full_message_id_;

  tl_object_ptr<td_api::paymentReceipt> payment_receipt_;

  void do_run(Promise<tl_object_ptr<td_api::paymentReceipt>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(payment_receipt_));
      return;
    }

    td->messages_manager_->get_payment_receipt(full_message_id_, std::move(promise));
  }

  void do_set_result(tl_object_ptr<td_api::paymentReceipt> &&result) override {
    payment_receipt_ = std::move(result);
  }

  void do_send_result() override {
    CHECK(payment_receipt_ != nullptr);
    send_result(std::move(payment_receipt_));
  }

 public:
  GetPaymentReceiptRequest(ActorShared<Td> td, uint64 request_id, int64 dialog_id, int64 message_id)
      : RequestActor(std::move(td), request_id), full_message_id_(DialogId(dialog_id), MessageId(message_id)) {
  }
};

class GetSavedOrderInfoRequest : public RequestActor<tl_object_ptr<td_api::orderInfo>> {
  tl_object_ptr<td_api::orderInfo> order_info_;

  void do_run(Promise<tl_object_ptr<td_api::orderInfo>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(order_info_));
      return;
    }

    get_saved_order_info(std::move(promise));
  }

  void do_set_result(tl_object_ptr<td_api::orderInfo> &&result) override {
    order_info_ = std::move(result);
  }

  void do_send_result() override {
    send_result(std::move(order_info_));
  }

 public:
  GetSavedOrderInfoRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class DeleteSavedOrderInfoRequest : public RequestOnceActor {
  void do_run(Promise<Unit> &&promise) override {
    delete_saved_order_info(std::move(promise));
  }

 public:
  DeleteSavedOrderInfoRequest(ActorShared<Td> td, uint64 request_id) : RequestOnceActor(std::move(td), request_id) {
  }
};

class DeleteSavedCredentialsRequest : public RequestOnceActor {
  void do_run(Promise<Unit> &&promise) override {
    delete_saved_credentials(std::move(promise));
  }

 public:
  DeleteSavedCredentialsRequest(ActorShared<Td> td, uint64 request_id) : RequestOnceActor(std::move(td), request_id) {
  }
};

class GetSupportUserRequest : public RequestActor<> {
  UserId user_id_;

  void do_run(Promise<Unit> &&promise) override {
    user_id_ = td->contacts_manager_->get_support_user(std::move(promise));
  }

  void do_send_result() override {
    send_result(td->contacts_manager_->get_user_object(user_id_));
  }

 public:
  GetSupportUserRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class GetWallpapersRequest : public RequestActor<tl_object_ptr<td_api::wallpapers>> {
  tl_object_ptr<td_api::wallpapers> wallpapers_;

  void do_run(Promise<tl_object_ptr<td_api::wallpapers>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(wallpapers_));
      return;
    }

    td->create_handler<GetWallpapersQuery>(std::move(promise))->send();
  }

  void do_set_result(tl_object_ptr<td_api::wallpapers> &&result) override {
    wallpapers_ = std::move(result);
  }

  void do_send_result() override {
    CHECK(wallpapers_ != nullptr);
    send_result(std::move(wallpapers_));
  }

 public:
  GetWallpapersRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class GetRecentlyVisitedTMeUrlsRequest : public RequestActor<tl_object_ptr<td_api::tMeUrls>> {
  string referrer_;

  tl_object_ptr<td_api::tMeUrls> urls_;

  void do_run(Promise<tl_object_ptr<td_api::tMeUrls>> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(urls_));
      return;
    }

    td->create_handler<GetRecentMeUrlsQuery>(std::move(promise))->send(referrer_);
  }

  void do_set_result(tl_object_ptr<td_api::tMeUrls> &&result) override {
    urls_ = std::move(result);
  }

  void do_send_result() override {
    CHECK(urls_ != nullptr);
    send_result(std::move(urls_));
  }

 public:
  GetRecentlyVisitedTMeUrlsRequest(ActorShared<Td> td, uint64 request_id, string referrer)
      : RequestActor(std::move(td), request_id), referrer_(std::move(referrer)) {
  }
};

class SendCustomRequestRequest : public RequestActor<string> {
  string method_;
  string parameters_;

  string request_result_;

  void do_run(Promise<string> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(request_result_));
      return;
    }

    td->create_handler<SendCustomRequestQuery>(std::move(promise))->send(method_, parameters_);
  }

  void do_set_result(string &&result) override {
    request_result_ = std::move(result);
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::customRequestResult>(request_result_));
  }

 public:
  SendCustomRequestRequest(ActorShared<Td> td, uint64 request_id, string &&method, string &&parameters)
      : RequestActor(std::move(td), request_id), method_(method), parameters_(parameters) {
  }
};

class AnswerCustomQueryRequest : public RequestOnceActor {
  int64 custom_query_id_;
  string data_;

  void do_run(Promise<Unit> &&promise) override {
    td->create_handler<AnswerCustomQueryQuery>(std::move(promise))->send(custom_query_id_, data_);
  }

 public:
  AnswerCustomQueryRequest(ActorShared<Td> td, uint64 request_id, int64 custom_query_id, string data)
      : RequestOnceActor(std::move(td), request_id), custom_query_id_(custom_query_id), data_(std::move(data)) {
  }
};

class GetInviteTextRequest : public RequestActor<string> {
  string text_;

  void do_run(Promise<string> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(text_));
      return;
    }

    td->create_handler<GetInviteTextQuery>(std::move(promise))->send();
  }

  void do_set_result(string &&result) override {
    text_ = std::move(result);
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::text>(text_));
  }

 public:
  GetInviteTextRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

class GetTermsOfServiceRequest : public RequestActor<string> {
  string text_;

  void do_run(Promise<string> &&promise) override {
    if (get_tries() < 2) {
      promise.set_value(std::move(text_));
      return;
    }

    td->create_handler<GetTermsOfServiceQuery>(std::move(promise))->send();
  }

  void do_set_result(string &&result) override {
    text_ = std::move(result);
  }

  void do_send_result() override {
    send_result(make_tl_object<td_api::text>(text_));
  }

 public:
  GetTermsOfServiceRequest(ActorShared<Td> td, uint64 request_id) : RequestActor(std::move(td), request_id) {
  }
};

/** Td **/
Td::Td(std::unique_ptr<TdCallback> callback) : callback_(std::move(callback)) {
}

void Td::on_alarm_timeout_callback(void *td_ptr, int64 request_id) {
  auto td = static_cast<Td *>(td_ptr);
  auto td_id = td->actor_id(td);
  send_closure_later(td_id, &Td::on_alarm_timeout, request_id);
}

void Td::on_alarm_timeout(int64 request_id) {
  if (request_id == 0) {
    on_online_updated(false, true);
    return;
  }
  send_result(static_cast<uint64>(request_id), make_tl_object<td_api::ok>());
}

void Td::on_online_updated(bool force, bool send_update) {
  if (close_flag_ >= 2 || auth_manager_->is_bot() || !auth_manager_->is_authorized()) {
    return;
  }
  if (force || is_online_) {
    contacts_manager_->set_my_online_status(is_online_, send_update);
    create_handler<UpdateStatusQuery>()->send(!is_online_);
  }
  if (is_online_) {
    alarm_timeout_.set_timeout_in(0, ONLINE_TIMEOUT);
  } else {
    alarm_timeout_.cancel_timeout(0);
  }
}

void Td::on_channel_unban_timeout(int64 channel_id_long) {
  if (close_flag_ >= 2) {
    return;
  }
  contacts_manager_->on_channel_unban_timeout(ChannelId(narrow_cast<int32>(channel_id_long)));
}

bool Td::is_online() const {
  return is_online_;
}

void Td::request(uint64 id, tl_object_ptr<td_api::Function> function) {
  request_set_.insert(id);

  if (id == 0) {
    LOG(ERROR) << "Receive request with id == 0";
    return send_error_raw(id, 400, "Wrong request id == 0");
  }
  if (function == nullptr) {
    LOG(ERROR) << "Receive empty request";
    return send_error_raw(id, 400, "Request is empty");
  }

  switch (state_) {
    case State::WaitParameters: {
      switch (function->get_id()) {
        case td_api::getAuthorizationState::ID:
          return send_result(id, td_api::make_object<td_api::authorizationStateWaitTdlibParameters>());
        case td_api::setTdlibParameters::ID:
          return answer_ok_query(
              id, set_td_parameters(std::move(move_tl_object_as<td_api::setTdlibParameters>(function)->parameters_)));
        default:
          return send_error_raw(id, 401, "Initialization parameters are needed");
      }
      break;
    }
    case State::Decrypt: {
      string encryption_key;
      switch (function->get_id()) {
        case td_api::getAuthorizationState::ID:
          return send_result(
              id, td_api::make_object<td_api::authorizationStateWaitEncryptionKey>(encryption_info_.is_encrypted));
        case td_api::checkDatabaseEncryptionKey::ID: {
          auto check_key = move_tl_object_as<td_api::checkDatabaseEncryptionKey>(function);
          encryption_key = std::move(check_key->encryption_key_);
          break;
        }
        case td_api::setDatabaseEncryptionKey::ID: {
          auto set_key = move_tl_object_as<td_api::setDatabaseEncryptionKey>(function);
          encryption_key = std::move(set_key->new_encryption_key_);
          break;
        }
        case td_api::close::ID:
          return close();
        case td_api::destroy::ID:
          return destroy();
        default:
          return send_error_raw(id, 401, "Database encryption key is needed");
      }
      return answer_ok_query(id, init(as_db_key(encryption_key)));
    }
    case State::Close: {
      if (function->get_id() == td_api::getAuthorizationState::ID) {
        if (close_flag_ == 5) {
          return send_result(id, td_api::make_object<td_api::authorizationStateClosed>());
        } else {
          return send_result(id, td_api::make_object<td_api::authorizationStateClosing>());
        }
      }
      return send_error_raw(id, 401, "Unauthorized");
    }
    case State::Run:
      break;
  }

  VLOG(td_requests) << "Receive request " << id << ": " << to_string(function);
  downcast_call(*function, [this, id](auto &request) { this->on_request(id, request); });
}

td_api::object_ptr<td_api::Object> Td::static_request(td_api::object_ptr<td_api::Function> function) {
  VLOG(td_requests) << "Receive static request: " << to_string(function);

  td_api::object_ptr<td_api::Object> response;
  downcast_call(*function, [&response](auto &request) { response = Td::do_static_request(request); });

  VLOG(td_requests) << "Sending result for static request: " << to_string(response);
  return response;
}

void Td::add_handler(uint64 id, std::shared_ptr<ResultHandler> handler) {
  result_handlers_.emplace_back(id, handler);
}

std::shared_ptr<Td::ResultHandler> Td::extract_handler(uint64 id) {
  std::shared_ptr<Td::ResultHandler> result;
  for (size_t i = 0; i < result_handlers_.size(); i++) {
    if (result_handlers_[i].first == id) {
      result = std::move(result_handlers_[i].second);
      result_handlers_.erase(result_handlers_.begin() + i);
      break;
    }
  }
  return result;
}

void Td::invalidate_handler(ResultHandler *handler) {
  for (size_t i = 0; i < result_handlers_.size(); i++) {
    if (result_handlers_[i].second.get() == handler) {
      result_handlers_.erase(result_handlers_.begin() + i);
      i--;
    }
  }
}

void Td::send(NetQueryPtr &&query) {
  VLOG(net_query) << "Send " << query << " to dispatcher";
  query->debug("Td: send to NetQueryDispatcher");
  query->set_callback(actor_shared(this, 1));
  G()->net_query_dispatcher().dispatch(std::move(query));
}

void Td::update_qts(int32 qts) {
  if (close_flag_ > 1) {
    return;
  }

  updates_manager_->set_qts(qts);
}

void Td::force_get_difference() {
  if (close_flag_) {
    return;
  }

  updates_manager_->get_difference("force_get_difference");
}

void Td::on_result(NetQueryPtr query) {
  query->debug("Td: received from DcManager");
  VLOG(net_query) << "on_result " << query;
  if (close_flag_ > 1) {
    return;
  }

  if (query->id() == 0) {
    if (query->is_error()) {
      query->clear();
      updates_manager_->schedule_get_difference("error in update");
      LOG(ERROR) << "Error in update";
      return;
    }
    auto ok = query->move_as_ok();
    TlBufferParser parser(&ok);
    auto ptr = telegram_api::Updates::fetch(parser);
    if (parser.get_error()) {
      LOG(ERROR) << "Failed to fetch update: " << parser.get_error() << format::as_hex_dump<4>(ok.as_slice());
      updates_manager_->schedule_get_difference("failed to fetch update");
    } else {
      updates_manager_->on_get_updates(std::move(ptr));
    }
    return;
  }
  auto handler = extract_handler(query->id());
  if (handler == nullptr) {
    query->clear();
    LOG_IF(WARNING, !query->is_ok() || query->ok_tl_constructor() != telegram_api::upload_file::ID)
        << tag("NetQuery", query) << " is ignored: no handlers found";
    return;
  }
  handler->on_result(std::move(query));
}

void Td::on_config_option_updated(const string &name) {
  if (close_flag_) {
    return;
  }
  if (name == "auth") {
    on_authorization_lost();
    return;
  } else if (name == "saved_animations_limit") {
    return animations_manager_->on_update_saved_animations_limit(G()->shared_config().get_option_integer(name));
  } else if (name == "favorite_stickers_limit") {
    stickers_manager_->on_update_favorite_stickers_limit(G()->shared_config().get_option_integer(name));
  } else if (name == "my_id") {
    G()->set_my_id(G()->shared_config().get_option_integer(name));
  } else if (name == "session_count") {
    G()->net_query_dispatcher().update_session_count();
  } else if (name == "use_pfs") {
    G()->net_query_dispatcher().update_use_pfs();
  } else if (name == "use_storage_optimizer") {
    send_closure(storage_manager_, &StorageManager::update_use_storage_optimizer);
  } else if (name == "rating_e_decay") {
    return send_closure(top_dialog_manager_, &TopDialogManager::update_rating_e_decay);
  } else if (name == "call_ring_timeout_ms" || name == "call_receive_timeout_ms" ||
             name == "channels_read_media_period") {
    return;
  }
  send_update(make_tl_object<td_api::updateOption>(name, G()->shared_config().get_option_value(name)));
}

tl_object_ptr<td_api::ConnectionState> Td::get_connection_state_object(StateManager::State state) {
  switch (state) {
    case StateManager::State::Empty:
      UNREACHABLE();
      return nullptr;
    case StateManager::State::WaitingForNetwork:
      return make_tl_object<td_api::connectionStateWaitingForNetwork>();
    case StateManager::State::ConnectingToProxy:
      return make_tl_object<td_api::connectionStateConnectingToProxy>();
    case StateManager::State::Connecting:
      return make_tl_object<td_api::connectionStateConnecting>();
    case StateManager::State::Updating:
      return make_tl_object<td_api::connectionStateUpdating>();
    case StateManager::State::Ready:
      return make_tl_object<td_api::connectionStateReady>();
    default:
      UNREACHABLE();
      return nullptr;
  }
}

void Td::on_connection_state_changed(StateManager::State new_state) {
  if (new_state == connection_state_) {
    LOG(ERROR) << "State manager sends update about unchanged state " << static_cast<int32>(new_state);
    return;
  }
  connection_state_ = new_state;

  send_update(make_tl_object<td_api::updateConnectionState>(get_connection_state_object(connection_state_)));
}

void Td::on_authorization_lost() {
  LOG(WARNING) << "on_authorization_lost";
  destroy();
}

void Td::start_up() {
  always_wait_for_mailbox();

  uint64 check_endianness = 0x0706050403020100;
  auto check_endianness_raw = reinterpret_cast<const unsigned char *>(&check_endianness);
  for (unsigned char c = 0; c < 8; c++) {
    auto symbol = check_endianness_raw[static_cast<size_t>(c)];
    LOG_IF(FATAL, symbol != c) << "TDLib requires little-endian platform";
  }

  CHECK(state_ == State::WaitParameters);
  send_update(td_api::make_object<td_api::updateAuthorizationState>(
      td_api::make_object<td_api::authorizationStateWaitTdlibParameters>()));
}

void Td::tear_down() {
  CHECK(close_flag_ == 5);
}

void Td::hangup_shared() {
  auto token = get_link_token();
  auto type = Container<int>::type_from_id(token);

  if (type == RequestActorIdType) {
    request_actors_.erase(get_link_token());
    dec_request_actor_refcnt();
  } else if (type == ActorIdType) {
    dec_actor_refcnt();
  } else {
    LOG(FATAL, "Unknown hangup_shared ") << tag("type", type);
  }
}

void Td::hangup() {
  close();
  dec_stop_cnt();
}

ActorShared<Td> Td::create_reference() {
  inc_actor_refcnt();
  return actor_shared(this, ActorIdType);
}
void Td::inc_actor_refcnt() {
  actor_refcnt_++;
}

void Td::dec_actor_refcnt() {
  actor_refcnt_--;
  if (actor_refcnt_ == 0) {
    if (close_flag_ == 2) {
      create_reference();
      close_flag_ = 3;
    } else if (close_flag_ == 3) {
      LOG(WARNING) << "ON_ACTORS_CLOSED";
      Timer timer;
      animations_manager_.reset();
      LOG(DEBUG) << "AnimationsManager was cleared " << timer;
      audios_manager_.reset();
      LOG(DEBUG) << "AudiosManager was cleared " << timer;
      auth_manager_.reset();
      LOG(DEBUG) << "AuthManager was cleared " << timer;
      change_phone_number_manager_.reset();
      LOG(DEBUG) << "ChangePhoneNumberManager was cleared " << timer;
      contacts_manager_.reset();
      LOG(DEBUG) << "ContactsManager was cleared " << timer;
      documents_manager_.reset();
      LOG(DEBUG) << "DocumentsManager was cleared " << timer;
      file_manager_.reset();
      LOG(DEBUG) << "FileManager was cleared " << timer;
      inline_queries_manager_.reset();
      LOG(DEBUG) << "InlineQueriesManager was cleared " << timer;
      messages_manager_.reset();
      LOG(DEBUG) << "MessagesManager was cleared " << timer;
      stickers_manager_.reset();
      LOG(DEBUG) << "StickersManager was cleared " << timer;
      updates_manager_.reset();
      LOG(DEBUG) << "UpdatesManager was cleared " << timer;
      video_notes_manager_.reset();
      LOG(DEBUG) << "VideoNotesManager was cleared " << timer;
      videos_manager_.reset();
      LOG(DEBUG) << "VideosManager was cleared " << timer;
      voice_notes_manager_.reset();
      LOG(DEBUG) << "VoiceNotesManager was cleared " << timer;
      web_pages_manager_.reset();
      LOG(DEBUG) << "WebPagesManager was cleared " << timer;
      Promise<> promise = PromiseCreator::lambda([actor_id = create_reference()](Unit) mutable { actor_id.reset(); });

      if (destroy_flag_) {
        G()->close_and_destroy_all(std::move(promise));
      } else {
        G()->close_all(std::move(promise));
      }
      // NetQueryDispatcher will be closed automatically
      close_flag_ = 4;
    } else if (close_flag_ == 4) {
      LOG(WARNING) << "ON_CLOSED";
      close_flag_ = 5;
      send_update(td_api::make_object<td_api::updateAuthorizationState>(
          td_api::make_object<td_api::authorizationStateClosed>()));
      callback_->on_closed();
      dec_stop_cnt();
    } else {
      UNREACHABLE();
    }
  }
}

void Td::dec_stop_cnt() {
  stop_cnt_--;
  if (stop_cnt_ == 0) {
    stop();
  }
}

void Td::inc_request_actor_refcnt() {
  request_actor_refcnt_++;
}

void Td::dec_request_actor_refcnt() {
  request_actor_refcnt_--;
  if (request_actor_refcnt_ == 0) {
    LOG(WARNING) << "no request actors";
    clear();
    dec_actor_refcnt();  // remove guard
  }
}

void Td::clear_handlers() {
  result_handlers_.clear();
}

void Td::clear() {
  if (close_flag_ >= 2) {
    return;
  }

  close_flag_ = 2;

  Timer timer;
  if (destroy_flag_) {
    for (auto &option : G()->shared_config().get_options()) {
      if (option.first == "rating_e_decay" || option.first == "saved_animations_limit" ||
          option.first == "call_receive_timeout_ms" || option.first == "call_ring_timeout_ms" ||
          option.first == "channels_read_media_period" || option.first == "auth") {
        continue;
      }
      send_update(make_tl_object<td_api::updateOption>(option.first, make_tl_object<td_api::optionValueEmpty>()));
    }
  }
  LOG(DEBUG) << "Options was cleared " << timer;

  G()->net_query_creator().stop_check();
  clear_handlers();
  LOG(DEBUG) << "Handlers was cleared " << timer;
  G()->net_query_dispatcher().stop();
  LOG(DEBUG) << "NetQueryDispatcher was stopped " << timer;
  state_manager_.reset();
  LOG(DEBUG) << "StateManager was cleared " << timer;
  while (!request_set_.empty()) {
    uint64 id = *request_set_.begin();
    if (destroy_flag_) {
      send_error_raw(id, 401, "Unauthorized");
    } else {
      send_error_raw(id, 500, "Internal Server Error: closing");
    }
    alarm_timeout_.cancel_timeout(static_cast<int64>(id));
  }
  if (is_online_) {
    is_online_ = false;
    alarm_timeout_.cancel_timeout(0);
  }
  LOG(DEBUG) << "Requests was answered " << timer;

  // close all pure actors
  call_manager_.reset();
  LOG(DEBUG) << "CallManager was cleared " << timer;
  config_manager_.reset();
  LOG(DEBUG) << "ConfigManager was cleared " << timer;
  device_token_manager_.reset();
  LOG(DEBUG) << "DeviceTokenManager was cleared " << timer;
  hashtag_hints_.reset();
  LOG(DEBUG) << "HashtagHints was cleared " << timer;
  net_stats_manager_.reset();
  LOG(DEBUG) << "NetStatsManager was cleared " << timer;
  password_manager_.reset();
  LOG(DEBUG) << "PasswordManager was cleared " << timer;
  privacy_manager_.reset();
  LOG(DEBUG) << "PrivacyManager was cleared " << timer;
  secret_chats_manager_.reset();
  LOG(DEBUG) << "SecretChatsManager was cleared " << timer;
  storage_manager_.reset();
  LOG(DEBUG) << "StorageManager was cleared " << timer;
  top_dialog_manager_.reset();
  LOG(DEBUG) << "TopDialogManager was cleared " << timer;

  G()->set_connection_creator(ActorOwn<ConnectionCreator>());
  LOG(DEBUG) << "ConnectionCreator was cleared " << timer;

  // clear actors which are unique pointers
  animations_manager_actor_.reset();
  LOG(DEBUG) << "AnimationsManager actor was cleared " << timer;
  auth_manager_actor_.reset();
  LOG(DEBUG) << "AuthManager actor was cleared " << timer;
  change_phone_number_manager_actor_.reset();
  LOG(DEBUG) << "ChangePhoneNumberManager actor was cleared " << timer;
  contacts_manager_actor_.reset();
  LOG(DEBUG) << "ContactsManager actor was cleared " << timer;
  file_manager_actor_.reset();
  LOG(DEBUG) << "FileManager actor was cleared " << timer;
  inline_queries_manager_actor_.reset();
  LOG(DEBUG) << "InlineQueriesManager actor was cleared " << timer;
  messages_manager_actor_.reset();  // TODO: Stop silent
  LOG(DEBUG) << "MessagesManager actor was cleared " << timer;
  stickers_manager_actor_.reset();
  LOG(DEBUG) << "StickersManager actor was cleared " << timer;
  updates_manager_actor_.reset();
  LOG(DEBUG) << "UpdatesManager actor was cleared " << timer;
  web_pages_manager_actor_.reset();
  LOG(DEBUG) << "WebPagesManager actor was cleared " << timer;
}

void Td::close() {
  close_impl(false);
}

void Td::destroy() {
  close_impl(true);
}

void Td::close_impl(bool destroy_flag) {
  destroy_flag_ |= destroy_flag;
  if (close_flag_) {
    return;
  }
  if (state_ == State::Decrypt) {
    if (destroy_flag) {
      TdDb::destroy(parameters_);
    }
    state_ = State::Close;
    close_flag_ = 4;
    return dec_actor_refcnt();
  }
  state_ = State::Close;
  close_flag_ = 1;
  G()->set_close_flag();
  send_closure(auth_manager_actor_, &AuthManager::on_closing);
  LOG(WARNING) << "Close " << tag("destroy", destroy_flag);

  // wait till all request_actors will stop.
  request_actors_.clear();
  G()->td_db()->flush_all();
  send_closure_later(actor_id(this), &Td::dec_request_actor_refcnt);  // remove guard
}

class Td::DownloadFileCallback : public FileManager::DownloadCallback {
 public:
  void on_progress(FileId file_id) override {
  }

  void on_download_ok(FileId file_id) override {
  }

  void on_download_error(FileId file_id, Status error) override {
  }
};

class Td::UploadFileCallback : public FileManager::UploadCallback {
 public:
  void on_progress(FileId file_id) override {
  }

  void on_upload_ok(FileId file_id, tl_object_ptr<telegram_api::InputFile> input_file) override {
    // cancel file upload of the file to allow next upload with the same file to succeed
    send_closure(G()->file_manager(), &FileManager::upload, file_id, nullptr, 0, 0);
  }

  void on_upload_encrypted_ok(FileId file_id, tl_object_ptr<telegram_api::InputEncryptedFile> input_file) override {
    // cancel file upload of the file to allow next upload with the same file to succeed
    send_closure(G()->file_manager(), &FileManager::upload, file_id, nullptr, 0, 0);
  }

  void on_upload_error(FileId file_id, Status error) override {
  }
};

Status Td::init(DbKey key) {
  auto current_scheduler_id = Scheduler::instance()->sched_id();
  auto scheduler_count = Scheduler::instance()->sched_count();

  TdDb::Events events;
  TRY_RESULT(td_db,
             TdDb::open(std::min(current_scheduler_id + 1, scheduler_count - 1), parameters_, std::move(key), events));
  LOG(INFO) << "Successfully inited database in " << tag("database_directory", parameters_.database_directory)
            << " and " << tag("files_directory", parameters_.files_directory);
  G()->init(parameters_, actor_id(this), std::move(td_db)).ensure();

  // Init all managers and actors
  class StateManagerCallback : public StateManager::Callback {
   public:
    explicit StateManagerCallback(ActorShared<Td> td) : td_(std::move(td)) {
    }
    bool on_state(StateManager::State state) override {
      send_closure(td_, &Td::on_connection_state_changed, state);
      return td_.is_alive();
    }

   private:
    ActorShared<Td> td_;
  };
  state_manager_ = create_actor<StateManager>("State manager");
  send_closure(state_manager_, &StateManager::add_callback, make_unique<StateManagerCallback>(create_reference()));
  G()->set_state_manager(state_manager_.get());
  connection_state_ = StateManager::State::Empty;

  {
    auto connection_creator = create_actor<ConnectionCreator>("ConnectionCreator", create_reference());
    auto net_stats_manager = create_actor<NetStatsManager>("NetStatsManager", create_reference());

    // How else could I let two actor know about each other, without quite complex async logic?
    auto net_stats_manager_ptr = net_stats_manager->get_actor_unsafe();
    net_stats_manager_ptr->init();
    connection_creator->get_actor_unsafe()->set_net_stats_callback(net_stats_manager_ptr->get_common_stats_callback(),
                                                                   net_stats_manager_ptr->get_media_stats_callback());
    G()->set_net_stats_file_callbacks(net_stats_manager_ptr->get_file_stats_callbacks());

    G()->set_connection_creator(std::move(connection_creator));
    net_stats_manager_ = std::move(net_stats_manager);
  }

  auto temp_auth_key_watchdog = create_actor<TempAuthKeyWatchdog>("TempAuthKeyWatchdog");
  G()->set_temp_auth_key_watchdog(std::move(temp_auth_key_watchdog));

  // create ConfigManager and ConfigShared
  class ConfigSharedCallback : public ConfigShared::Callback {
   public:
    void on_option_updated(const string &name) override {
      send_closure(G()->td(), &Td::on_config_option_updated, name);
    }
  };
  send_update(
      make_tl_object<td_api::updateOption>("version", make_tl_object<td_api::optionValueString>(tdlib_version)));

  G()->set_shared_config(
      std::make_unique<ConfigShared>(G()->td_db()->get_config_pmc(), std::make_unique<ConfigSharedCallback>()));
  config_manager_ = create_actor<ConfigManager>("ConfigManager", create_reference());
  G()->set_config_manager(config_manager_.get());

  auto net_query_dispatcher = std::make_unique<NetQueryDispatcher>([&] { return create_reference(); });
  G()->set_net_query_dispatcher(std::move(net_query_dispatcher));

  auth_manager_ = std::make_unique<AuthManager>(parameters_.api_id, parameters_.api_hash, create_reference());
  auth_manager_actor_ = register_actor("AuthManager", auth_manager_.get());

  download_file_callback_ = std::make_shared<DownloadFileCallback>();
  upload_file_callback_ = std::make_shared<UploadFileCallback>();

  class FileManagerContext : public FileManager::Context {
   public:
    explicit FileManagerContext(Td *td) : td_(td) {
    }
    void on_new_file(int64 size) final {
      send_closure(G()->storage_manager(), &StorageManager::on_new_file, size);
    }
    void on_file_updated(FileId file_id) final {
      send_closure(G()->td(), &Td::send_update,
                   make_tl_object<td_api::updateFile>(td_->file_manager_->get_file_object(file_id)));
    }
    ActorShared<> create_reference() final {
      return td_->create_reference();
    }

   private:
    Td *td_;
  };
  file_manager_ = std::make_unique<FileManager>(std::make_unique<FileManagerContext>(this));
  file_manager_actor_ = register_actor("FileManager", file_manager_.get());
  file_manager_->init_actor();
  G()->set_file_manager(file_manager_actor_.get());

  audios_manager_ = make_unique<AudiosManager>(this);
  callback_queries_manager_ = make_unique<CallbackQueriesManager>(this);
  documents_manager_ = make_unique<DocumentsManager>(this);
  video_notes_manager_ = make_unique<VideoNotesManager>(this);
  videos_manager_ = make_unique<VideosManager>(this);
  voice_notes_manager_ = make_unique<VoiceNotesManager>(this);

  animations_manager_ = std::make_unique<AnimationsManager>(this, create_reference());
  animations_manager_actor_ = register_actor("AnimationsManager", animations_manager_.get());
  G()->set_animations_manager(animations_manager_actor_.get());
  change_phone_number_manager_ = std::make_unique<ChangePhoneNumberManager>(create_reference());
  change_phone_number_manager_actor_ = register_actor("ChangePhoneNumberManager", change_phone_number_manager_.get());
  contacts_manager_ = std::make_unique<ContactsManager>(this, create_reference());
  contacts_manager_actor_ = register_actor("ContactsManager", contacts_manager_.get());
  G()->set_contacts_manager(contacts_manager_actor_.get());
  inline_queries_manager_ = std::make_unique<InlineQueriesManager>(this, create_reference());
  inline_queries_manager_actor_ = register_actor("InlineQueriesManager", inline_queries_manager_.get());
  messages_manager_ = std::make_unique<MessagesManager>(this, create_reference());
  messages_manager_actor_ = register_actor("MessagesManager", messages_manager_.get());
  G()->set_messages_manager(messages_manager_actor_.get());
  stickers_manager_ = std::make_unique<StickersManager>(this, create_reference());
  stickers_manager_actor_ = register_actor("StickersManager", stickers_manager_.get());
  G()->set_stickers_manager(stickers_manager_actor_.get());
  updates_manager_ = std::make_unique<UpdatesManager>(this, create_reference());
  updates_manager_actor_ = register_actor("UpdatesManager", updates_manager_.get());
  G()->set_updates_manager(updates_manager_actor_.get());
  web_pages_manager_ = std::make_unique<WebPagesManager>(this, create_reference());
  web_pages_manager_actor_ = register_actor("WebPagesManager", web_pages_manager_.get());
  G()->set_web_pages_manager(web_pages_manager_actor_.get());

  call_manager_ = create_actor<CallManager>("CallManager", create_reference());
  G()->set_call_manager(call_manager_.get());
  device_token_manager_ = create_actor<DeviceTokenManager>("DeviceTokenManager", create_reference());
  hashtag_hints_ = create_actor<HashtagHints>("HashtagHints", "text", create_reference());
  password_manager_ = create_actor<PasswordManager>("PasswordManager", create_reference());
  privacy_manager_ = create_actor<PrivacyManager>("PrivacyManager", create_reference());
  secret_chats_manager_ = create_actor<SecretChatsManager>("SecretChatsManager", create_reference());
  G()->set_secret_chats_manager(secret_chats_manager_.get());
  storage_manager_ = create_actor<StorageManager>("StorageManager", create_reference(),
                                                  std::min(current_scheduler_id + 2, scheduler_count - 1));
  G()->set_storage_manager(storage_manager_.get());
  top_dialog_manager_ = create_actor<TopDialogManager>("TopDialogManager", create_reference());
  G()->set_top_dialog_manager(top_dialog_manager_.get());

  for (auto &event : events.user_events) {
    contacts_manager_->on_binlog_user_event(std::move(event));
  }

  for (auto &event : events.chat_events) {
    contacts_manager_->on_binlog_chat_event(std::move(event));
  }

  for (auto &event : events.channel_events) {
    contacts_manager_->on_binlog_channel_event(std::move(event));
  }

  for (auto &event : events.secret_chat_events) {
    contacts_manager_->on_binlog_secret_chat_event(std::move(event));
  }

  for (auto &event : events.web_page_events) {
    web_pages_manager_->on_binlog_web_page_event(std::move(event));
  }

  // Send binlog events to managers
  //
  // 1. Actors must receive all binlog events before other queries.
  //
  // -- All actors have one "entry point". So there is only one way to send query to them. So all queries are ordered
  // for  each Actor.
  //
  //
  // 2. An actor must not make some decisions before all binlog events are processed.
  // For example, SecretChatActor must not send RequestKey, before it receives logevent with RequestKey and understands
  // that RequestKey was already sent.
  //
  // -- G()->wait_binlog_replay_finish(Promise<>);
  //
  // 3. During replay of binlog some queries may be sent to other actors. They shouldn't process such events before all
  // their binlog events are processed. So actor may receive some old queries. It must be in it's actual state in
  // orded to handle them properly.
  //
  // -- Use send_closure_later, so actors don't even start process binlog events, before all binlog events are sent

  for (auto &event : events.to_secret_chats_manager) {
    send_closure_later(secret_chats_manager_, &SecretChatsManager::replay_binlog_event, std::move(event));
  }

  send_closure_later(messages_manager_actor_, &MessagesManager::on_binlog_events,
                     std::move(events.to_messages_manager));

  // NB: be very careful. This notification may be received before all binlog events are.
  G()->on_binlog_replay_finish();
  send_closure(secret_chats_manager_, &SecretChatsManager::binlog_replay_finish);

  if (!auth_manager_->is_authorized()) {
    create_handler<GetNearestDcQuery>()->send();
  } else {
    updates_manager_->get_difference("init");
  }

  state_ = State::Run;
  return Status::OK();
}

void Td::send_update(tl_object_ptr<td_api::Update> &&object) {
  switch (object->get_id()) {
    case td_api::updateFavoriteStickers::ID:
    case td_api::updateInstalledStickerSets::ID:
    case td_api::updateRecentStickers::ID:
    case td_api::updateSavedAnimations::ID:
    case td_api::updateUserStatus::ID:
      VLOG(td_requests) << "Sending update: " << oneline(to_string(object));
      break;
    case td_api::updateTrendingStickerSets::ID:
      VLOG(td_requests) << "Sending update: updateTrendingStickerSets { ... }";
      break;
    default:
      VLOG(td_requests) << "Sending update: " << to_string(object);
  }

  callback_->on_result(0, std::move(object));
}

void Td::send_result(uint64 id, tl_object_ptr<td_api::Object> object) {
  LOG_IF(ERROR, id == 0) << "Sending " << to_string(object) << " through send_result";
  if (id == 0 || request_set_.erase(id)) {
    VLOG(td_requests) << "Sending result for request " << id << ": " << to_string(object);
    if (object == nullptr) {
      object = make_tl_object<td_api::error>(404, "Not Found");
    }
    callback_->on_result(id, std::move(object));
  }
}

void Td::send_error_impl(uint64 id, tl_object_ptr<td_api::error> error) {
  CHECK(id != 0);
  CHECK(callback_ != nullptr);
  CHECK(error != nullptr);
  if (request_set_.erase(id)) {
    VLOG(td_requests) << "Sending error for request " << id << ": " << oneline(to_string(error));
    callback_->on_error(id, std::move(error));
  }
}

void Td::send_error(uint64 id, Status error) {
  send_error_impl(id, make_tl_object<td_api::error>(error.code(), error.message().str()));
  error.ignore();
}

namespace {
auto create_error_raw(int32 code, CSlice error) {
  return make_tl_object<td_api::error>(code, error.str());
}
}  // namespace

void Td::send_error_raw(uint64 id, int32 code, CSlice error) {
  send_error_impl(id, create_error_raw(code, error));
}

void Td::answer_ok_query(uint64 id, Status status) {
  if (status.is_error()) {
    send_closure(actor_id(this), &Td::send_error, id, std::move(status));
  } else {
    send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
  }
}

#define CLEAN_INPUT_STRING(field_name)                                  \
  if (!clean_input_string(field_name)) {                                \
    return send_error_raw(id, 400, "Strings must be encoded in UTF-8"); \
  }
#define CHECK_AUTH()                                \
  if (!auth_manager_->is_authorized()) {            \
    return send_error_raw(id, 401, "Unauthorized"); \
  }
#define CHECK_IS_BOT()                                              \
  if (!auth_manager_->is_bot()) {                                   \
    return send_error_raw(id, 400, "Only bots can use the method"); \
  }
#define CHECK_IS_USER()                                                     \
  if (auth_manager_->is_bot()) {                                            \
    return send_error_raw(id, 400, "The method is not available for bots"); \
  }

#define CREATE_NO_ARGS_REQUEST(name)                                       \
  auto slot_id = request_actors_.create(ActorOwn<>(), RequestActorIdType); \
  inc_request_actor_refcnt();                                              \
  *request_actors_.get(slot_id) = create_actor<name>(#name, actor_shared(this, slot_id), id);
#define CREATE_REQUEST(name, ...)                                          \
  auto slot_id = request_actors_.create(ActorOwn<>(), RequestActorIdType); \
  inc_request_actor_refcnt();                                              \
  *request_actors_.get(slot_id) = create_actor<name>(#name, actor_shared(this, slot_id), id, __VA_ARGS__);
#define CREATE_REQUEST_PROMISE(name) \
  auto name = create_request_promise<std::decay_t<decltype(request)>::ReturnType>(id);

Status Td::fix_parameters(TdParameters &parameters) {
  if (parameters.database_directory.empty()) {
    parameters.database_directory = ".";
  }
  if (parameters.files_directory.empty()) {
    parameters.files_directory = parameters.database_directory;
  }
  if (parameters.use_message_db) {
    parameters.use_chat_info_db = true;
  }
  if (parameters.use_chat_info_db) {
    parameters.use_file_db = true;
  }
  if (parameters.api_id == 0) {
    return Status::Error(400, "Valid api_id must be provided. Can be obtained at https://my.telegram.org");
  }
  if (parameters.api_hash.empty()) {
    return Status::Error(400, "Valid api_hash must be provided. Can be obtained at https://my.telegram.org");
  }

  auto prepare_dir = [](string dir) -> Result<string> {
    CHECK(!dir.empty());
    if (dir.back() != TD_DIR_SLASH) {
      dir += TD_DIR_SLASH;
    }
    TRY_STATUS(mkpath(dir, 0750));
    TRY_RESULT(real_dir, realpath(dir, true));
    if (dir.back() != TD_DIR_SLASH) {
      dir += TD_DIR_SLASH;
    }
    return real_dir;
  };

  auto r_database_directory = prepare_dir(parameters.database_directory);
  if (r_database_directory.is_error()) {
    return Status::Error(400, PSLICE() << "Can't init database in the directory \"" << parameters.database_directory
                                       << "\": " << r_database_directory.error());
  }
  parameters.database_directory = r_database_directory.move_as_ok();
  auto r_files_directory = prepare_dir(parameters.files_directory);
  if (r_files_directory.is_error()) {
    return Status::Error(400, PSLICE() << "Can't init files directory \"" << parameters.files_directory
                                       << "\": " << r_files_directory.error());
  }
  parameters.files_directory = r_files_directory.move_as_ok();

  return Status::OK();
}

Status Td::set_td_parameters(td_api::object_ptr<td_api::tdlibParameters> parameters) {
  if (!clean_input_string(parameters->api_hash_) && !clean_input_string(parameters->system_language_code_) &&
      !clean_input_string(parameters->device_model_) && !clean_input_string(parameters->system_version_) &&
      !clean_input_string(parameters->application_version_)) {
    return Status::Error(400, "Strings must be encoded in UTF-8");
  }

  parameters_.use_test_dc = parameters->use_test_dc_;
  parameters_.database_directory = parameters->database_directory_;
  parameters_.files_directory = parameters->files_directory_;
  parameters_.api_id = parameters->api_id_;
  parameters_.api_hash = parameters->api_hash_;
  parameters_.use_file_db = parameters->use_file_database_;
  parameters_.enable_storage_optimizer = parameters->enable_storage_optimizer_;
  parameters_.ignore_file_names = parameters->ignore_file_names_;
  parameters_.use_secret_chats = parameters->use_secret_chats_;
  parameters_.use_chat_info_db = parameters->use_chat_info_database_;
  parameters_.use_message_db = parameters->use_message_database_;

  TRY_STATUS(fix_parameters(parameters_));
  TRY_RESULT(encryption_info, TdDb::check_encryption(parameters_));
  encryption_info_ = std::move(encryption_info);

  alarm_timeout_.set_callback(on_alarm_timeout_callback);
  alarm_timeout_.set_callback_data(static_cast<void *>(this));

  set_context(std::make_shared<Global>());
  inc_request_actor_refcnt();  // guard
  inc_actor_refcnt();          // guard

  MtprotoHeader::Options options;
  options.api_id = parameters->api_id_;
  options.system_language_code = parameters->system_language_code_;
  options.device_model = parameters->device_model_;
  options.system_version = parameters->system_version_;
  options.application_version = parameters->application_version_;
  if (options.api_id != 21724) {
    options.application_version += ", TDLib ";
    options.application_version += tdlib_version;
  }
  G()->set_mtproto_header(std::make_unique<MtprotoHeader>(options));

  state_ = State::Decrypt;
  send_update(td_api::make_object<td_api::updateAuthorizationState>(
      td_api::make_object<td_api::authorizationStateWaitEncryptionKey>(encryption_info_.is_encrypted)));
  return Status::OK();
}

void Td::on_request(uint64 id, const td_api::setTdlibParameters &request) {
  send_error_raw(id, 400, "Unexpected setTdlibParameters");
}

void Td::on_request(uint64 id, const td_api::checkDatabaseEncryptionKey &request) {
  send_error_raw(id, 400, "Unexpected checkDatabaseEncryptionKey");
}

void Td::on_request(uint64 id, td_api::setDatabaseEncryptionKey &request) {
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  G()->td_db()->get_binlog()->change_key(as_db_key(std::move(request.new_encryption_key_)), std::move(query_promise));
}

void Td::on_request(uint64 id, const td_api::getAuthorizationState &request) {
  send_closure(auth_manager_actor_, &AuthManager::get_state, id);
}

void Td::on_request(uint64 id, td_api::setAuthenticationPhoneNumber &request) {
  CLEAN_INPUT_STRING(request.phone_number_);
  send_closure(auth_manager_actor_, &AuthManager::set_phone_number, id, std::move(request.phone_number_),
               request.allow_flash_call_, request.is_current_phone_number_);
}

void Td::on_request(uint64 id, const td_api::resendAuthenticationCode &) {
  send_closure(auth_manager_actor_, &AuthManager::resend_authentication_code, id);
}

void Td::on_request(uint64 id, td_api::checkAuthenticationCode &request) {
  CLEAN_INPUT_STRING(request.code_);
  CLEAN_INPUT_STRING(request.first_name_);
  CLEAN_INPUT_STRING(request.last_name_);
  send_closure(auth_manager_actor_, &AuthManager::check_code, id, std::move(request.code_),
               std::move(request.first_name_), std::move(request.last_name_));
}

void Td::on_request(uint64 id, td_api::checkAuthenticationPassword &request) {
  CLEAN_INPUT_STRING(request.password_);
  send_closure(auth_manager_actor_, &AuthManager::check_password, id, std::move(request.password_));
}

void Td::on_request(uint64 id, const td_api::requestAuthenticationPasswordRecovery &request) {
  send_closure(auth_manager_actor_, &AuthManager::request_password_recovery, id);
}

void Td::on_request(uint64 id, td_api::recoverAuthenticationPassword &request) {
  CLEAN_INPUT_STRING(request.recovery_code_);
  send_closure(auth_manager_actor_, &AuthManager::recover_password, id, std::move(request.recovery_code_));
}

void Td::on_request(uint64 id, const td_api::logOut &request) {
  // will call Td::destroy later
  send_closure(auth_manager_actor_, &AuthManager::logout, id);
}

void Td::on_request(uint64 id, const td_api::close &request) {
  close();
  send_result(id, td_api::make_object<td_api::ok>());
}

void Td::on_request(uint64 id, const td_api::destroy &request) {
  destroy();
  send_result(id, td_api::make_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::checkAuthenticationBotToken &request) {
  CLEAN_INPUT_STRING(request.token_);
  send_closure(auth_manager_actor_, &AuthManager::check_bot_token, id, std::move(request.token_));
}

void Td::on_request(uint64 id, td_api::getPasswordState &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::get_state, std::move(promise));
}

void Td::on_request(uint64 id, td_api::setPassword &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.old_password_);
  CLEAN_INPUT_STRING(request.new_password_);
  CLEAN_INPUT_STRING(request.new_hint_);
  CLEAN_INPUT_STRING(request.new_recovery_email_address_);
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::set_password, std::move(request.old_password_),
               std::move(request.new_password_), std::move(request.new_hint_), request.set_recovery_email_address_,
               std::move(request.new_recovery_email_address_), std::move(promise));
}

void Td::on_request(uint64 id, td_api::setRecoveryEmailAddress &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.password_);
  CLEAN_INPUT_STRING(request.new_recovery_email_address_);
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::set_recovery_email_address, std::move(request.password_),
               std::move(request.new_recovery_email_address_), std::move(promise));
}

void Td::on_request(uint64 id, td_api::getRecoveryEmailAddress &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.password_);
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::get_recovery_email_address, std::move(request.password_),
               std::move(promise));
}

void Td::on_request(uint64 id, td_api::requestPasswordRecovery &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::request_password_recovery, std::move(promise));
}

void Td::on_request(uint64 id, td_api::recoverPassword &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.recovery_code_);
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::recover_password, std::move(request.recovery_code_),
               std::move(promise));
}

void Td::on_request(uint64 id, td_api::getTemporaryPasswordState &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::get_temp_password_state, std::move(promise));
}

void Td::on_request(uint64 id, td_api::createTemporaryPassword &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.password_);
  CREATE_REQUEST_PROMISE(promise);
  send_closure(password_manager_, &PasswordManager::create_temp_password, std::move(request.password_),
               request.valid_for_, std::move(promise));
}

void Td::on_request(uint64 id, td_api::processDcUpdate &request) {
  CREATE_REQUEST_PROMISE(promise);
  CLEAN_INPUT_STRING(request.dc_);
  CLEAN_INPUT_STRING(request.addr_);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  auto dc_id_raw = to_integer<int32>(request.dc_);
  if (!DcId::is_valid(dc_id_raw)) {
    promise.set_error(Status::Error("Invalid dc id"));
    return;
  }
  send_closure(G()->connection_creator(), &ConnectionCreator::on_dc_update, DcId::internal(dc_id_raw), request.addr_,
               std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::registerDevice &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  if (request.device_token_ == nullptr) {
    return send_error_raw(id, 400, "Device token should not be empty");
  }

  CREATE_REQUEST_PROMISE(promise);
  send_closure(device_token_manager_, &DeviceTokenManager::register_device, std::move(request.device_token_),
               std::move(request.other_user_ids_), std::move(promise));
}

void Td::on_request(uint64 id, td_api::getUserPrivacySettingRules &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  send_closure(privacy_manager_, &PrivacyManager::get_privacy, std::move(request.setting_), std::move(promise));
}

void Td::on_request(uint64 id, td_api::setUserPrivacySettingRules &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  send_closure(privacy_manager_, &PrivacyManager::set_privacy, std::move(request.setting_), std::move(request.rules_),
               std::move(promise));
}

void Td::on_request(uint64 id, const td_api::getAccountTtl &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetAccountTtlRequest);
}

void Td::on_request(uint64 id, const td_api::setAccountTtl &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  if (request.ttl_ == nullptr) {
    return send_error_raw(id, 400, "New account TTL should not be empty");
  }
  CREATE_REQUEST(SetAccountTtlRequest, request.ttl_->days_);
}

void Td::on_request(uint64 id, td_api::deleteAccount &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.reason_);
  send_closure(auth_manager_actor_, &AuthManager::delete_account, id, request.reason_);
}

void Td::on_request(uint64 id, td_api::changePhoneNumber &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.phone_number_);
  change_phone_number_manager_->change_phone_number(id, std::move(request.phone_number_), request.allow_flash_call_,
                                                    request.is_current_phone_number_);
}

void Td::on_request(uint64 id, td_api::checkChangePhoneNumberCode &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.code_);
  change_phone_number_manager_->check_code(id, std::move(request.code_));
}

void Td::on_request(uint64 id, td_api::resendChangePhoneNumberCode &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  change_phone_number_manager_->resend_authentication_code(id);
}

void Td::on_request(uint64 id, const td_api::getActiveSessions &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetActiveSessionsRequest);
}

void Td::on_request(uint64 id, const td_api::terminateSession &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(TerminateSessionRequest, request.session_id_);
}

void Td::on_request(uint64 id, const td_api::terminateAllOtherSessions &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(TerminateAllOtherSessionsRequest);
}

void Td::on_request(uint64 id, const td_api::getMe &) {
  CHECK_AUTH();

  UserId my_id = contacts_manager_->get_my_id("getMe");
  CHECK(my_id.is_valid());
  CREATE_REQUEST(GetUserRequest, my_id.get());
}

void Td::on_request(uint64 id, const td_api::getUser &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetUserRequest, request.user_id_);
}

void Td::on_request(uint64 id, const td_api::getUserFullInfo &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetUserFullInfoRequest, request.user_id_);
}

void Td::on_request(uint64 id, const td_api::getBasicGroup &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetGroupRequest, request.basic_group_id_);
}

void Td::on_request(uint64 id, const td_api::getBasicGroupFullInfo &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetGroupFullInfoRequest, request.basic_group_id_);
}

void Td::on_request(uint64 id, const td_api::getSupergroup &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetSupergroupRequest, request.supergroup_id_);
}

void Td::on_request(uint64 id, const td_api::getSupergroupFullInfo &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetSupergroupFullInfoRequest, request.supergroup_id_);
}

void Td::on_request(uint64 id, const td_api::getSecretChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetSecretChatRequest, request.secret_chat_id_);
}

void Td::on_request(uint64 id, const td_api::getChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetChatRequest, request.chat_id_);
}

void Td::on_request(uint64 id, const td_api::getMessage &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetMessageRequest, request.chat_id_, request.message_id_);
}

void Td::on_request(uint64 id, const td_api::getMessages &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetMessagesRequest, request.chat_id_, request.message_ids_);
}

void Td::on_request(uint64 id, const td_api::getPublicMessageLink &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetPublicMessageLinkRequest, request.chat_id_, request.message_id_, request.for_album_);
}

void Td::on_request(uint64 id, const td_api::getFile &request) {
  CHECK_AUTH();
  send_closure(actor_id(this), &Td::send_result, id, file_manager_->get_file_object(FileId(request.file_id_)));
}

void Td::on_request(uint64 id, td_api::getRemoteFile &request) {
  CHECK_AUTH();
  CLEAN_INPUT_STRING(request.remote_file_id_);
  auto r_file_id = file_manager_->from_persistent_id(
      request.remote_file_id_, request.file_type_ == nullptr ? FileType::Temp : from_td_api(*request.file_type_));
  if (r_file_id.is_error()) {
    auto error = r_file_id.move_as_error();
    send_closure(actor_id(this), &Td::send_error, id, std::move(error));
  } else {
    send_closure(actor_id(this), &Td::send_result, id, file_manager_->get_file_object(r_file_id.ok()));
  }
}

void Td::on_request(uint64 id, td_api::getStorageStatistics &request) {
  CHECK_AUTH();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<FileStats> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(result.ok().as_td_api());
    }
  });
  send_closure(storage_manager_, &StorageManager::get_storage_stats, request.chat_limit_, std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::getStorageStatisticsFast &request) {
  CHECK_AUTH();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<FileStatsFast> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(result.ok().as_td_api());
    }
  });
  send_closure(storage_manager_, &StorageManager::get_storage_stats_fast, std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::optimizeStorage &request) {
  CHECK_AUTH();
  std::vector<FileType> file_types;
  for (auto &file_type : request.file_types_) {
    if (file_type == nullptr) {
      return send_error_raw(id, 400, "File type should not be empty");
    }

    file_types.push_back(from_td_api(*file_type));
  }
  std::vector<DialogId> owner_dialog_ids;
  for (auto chat_id : request.chat_ids_) {
    DialogId dialog_id(chat_id);
    if (!dialog_id.is_valid() && dialog_id != DialogId()) {
      return send_error_raw(id, 400, "Wrong chat id");
    }
    owner_dialog_ids.push_back(dialog_id);
  }
  std::vector<DialogId> exclude_owner_dialog_ids;
  for (auto chat_id : request.exclude_chat_ids_) {
    DialogId dialog_id(chat_id);
    if (!dialog_id.is_valid() && dialog_id != DialogId()) {
      return send_error_raw(id, 400, "Wrong chat id");
    }
    exclude_owner_dialog_ids.push_back(dialog_id);
  }
  FileGcParameters parameters(request.size_, request.ttl_, request.count_, request.immunity_delay_,
                              std::move(file_types), std::move(owner_dialog_ids), std::move(exclude_owner_dialog_ids),
                              request.chat_limit_);

  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<FileStats> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(result.ok().as_td_api());
    }
  });
  send_closure(storage_manager_, &StorageManager::run_gc, std::move(parameters), std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::getNetworkStatistics &request) {
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<NetworkStats> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(result.ok().as_td_api());
    }
  });
  send_closure(net_stats_manager_, &NetStatsManager::get_network_stats, request.only_current_,
               std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::resetNetworkStatistics &request) {
  CREATE_REQUEST_PROMISE(promise);
  send_closure(net_stats_manager_, &NetStatsManager::reset_network_stats);
  promise.set_value(make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::addNetworkStatistics &request) {
  CREATE_REQUEST_PROMISE(promise);
  if (request.entry_ == nullptr) {
    return send_error_raw(id, 400, "Network statistics entry should not be empty");
  }

  NetworkStatsEntry entry;
  switch (request.entry_->get_id()) {
    case td_api::networkStatisticsEntryFile::ID: {
      auto file_entry = move_tl_object_as<td_api::networkStatisticsEntryFile>(request.entry_);
      entry.is_call = false;
      if (file_entry->file_type_ != nullptr) {
        entry.file_type = from_td_api(*file_entry->file_type_);
      }
      entry.net_type = from_td_api(file_entry->network_type_);
      entry.rx = file_entry->received_bytes_;
      entry.tx = file_entry->sent_bytes_;
      break;
    }
    case td_api::networkStatisticsEntryCall::ID: {
      auto call_entry = move_tl_object_as<td_api::networkStatisticsEntryCall>(request.entry_);
      entry.is_call = true;
      entry.net_type = from_td_api(call_entry->network_type_);
      entry.rx = call_entry->received_bytes_;
      entry.tx = call_entry->sent_bytes_;
      entry.duration = call_entry->duration_;
      break;
    }
    default:
      UNREACHABLE();
  }

  if (entry.net_type == NetType::None) {
    return send_error_raw(id, 400, "Network statistics entry can't be increased for NetworkTypeNone");
  }
  if (entry.rx > (1ll << 40) || entry.rx < 0) {
    return send_error_raw(id, 400, "Wrong received bytes value");
  }
  if (entry.tx > (1ll << 40) || entry.tx < 0) {
    return send_error_raw(id, 400, "Wrong sent bytes value");
  }
  if (entry.count > (1 << 30) || entry.count < 0) {
    return send_error_raw(id, 400, "Wrong count value");
  }
  if (entry.duration > (1 << 30) || entry.duration < 0) {
    return send_error_raw(id, 400, "Wrong duration value");
  }

  send_closure(net_stats_manager_, &NetStatsManager::add_network_stats, entry);
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::setNetworkType &request) {
  CREATE_REQUEST_PROMISE(promise);
  send_closure(state_manager_, &StateManager::on_network, from_td_api(request.type_));
  promise.set_value(make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::getTopChats &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  if (request.category_ == nullptr) {
    promise.set_error(Status::Error(400, "Top chat category should not be empty"));
    return;
  }
  if (request.limit_ <= 0) {
    promise.set_error(Status::Error(400, "Limit must be positive"));
    return;
  }
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<vector<DialogId>> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(MessagesManager::get_chats_object(result.ok()));
    }
  });
  send_closure(top_dialog_manager_, &TopDialogManager::get_top_dialogs,
               top_dialog_category_from_td_api(*request.category_), request.limit_, std::move(query_promise));
}

void Td::on_request(uint64 id, const td_api::removeTopChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  if (request.category_ == nullptr) {
    return send_error_raw(id, 400, "Top chat category should not be empty");
  }

  send_closure(top_dialog_manager_, &TopDialogManager::remove_dialog,
               top_dialog_category_from_td_api(*request.category_), DialogId(request.chat_id_),
               messages_manager_->get_input_peer(DialogId(request.chat_id_), AccessRights::Read));
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, const td_api::getChats &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetChatsRequest, request.offset_order_, request.offset_chat_id_, request.limit_);
}

void Td::on_request(uint64 id, td_api::searchPublicChat &request) {
  CHECK_AUTH();
  CLEAN_INPUT_STRING(request.username_);
  CREATE_REQUEST(SearchPublicChatRequest, request.username_);
}

void Td::on_request(uint64 id, td_api::searchPublicChats &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(SearchPublicChatsRequest, request.query_);
}

void Td::on_request(uint64 id, td_api::searchChats &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(SearchChatsRequest, request.query_, request.limit_);
}

void Td::on_request(uint64 id, td_api::searchChatsOnServer &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(SearchChatsOnServerRequest, request.query_, request.limit_);
}

void Td::on_request(uint64 id, const td_api::getGroupsInCommon &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetGroupsInCommonRequest, request.user_id_, request.offset_chat_id_, request.limit_);
}

void Td::on_request(uint64 id, const td_api::getCreatedPublicChats &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetCreatedPublicChatsRequest);
}

void Td::on_request(uint64 id, const td_api::addRecentlyFoundChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, messages_manager_->add_recently_found_dialog(DialogId(request.chat_id_)));
}

void Td::on_request(uint64 id, const td_api::removeRecentlyFoundChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, messages_manager_->remove_recently_found_dialog(DialogId(request.chat_id_)));
}

void Td::on_request(uint64 id, const td_api::clearRecentlyFoundChats &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  messages_manager_->clear_recently_found_dialogs();
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, const td_api::openChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, messages_manager_->open_dialog(DialogId(request.chat_id_)));
}

void Td::on_request(uint64 id, const td_api::closeChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, messages_manager_->close_dialog(DialogId(request.chat_id_)));
}

void Td::on_request(uint64 id, const td_api::viewMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(
      id, messages_manager_->view_messages(
              DialogId(request.chat_id_), MessagesManager::get_message_ids(request.message_ids_), request.force_read_));
}

void Td::on_request(uint64 id, const td_api::openMessageContent &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(
      id, messages_manager_->open_message_content({DialogId(request.chat_id_), MessageId(request.message_id_)}));
}

void Td::on_request(uint64 id, const td_api::getChatHistory &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetChatHistoryRequest, request.chat_id_, request.from_message_id_, request.offset_, request.limit_,
                 request.only_local_);
}

void Td::on_request(uint64 id, const td_api::deleteChatHistory &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(DeleteChatHistoryRequest, request.chat_id_, request.remove_from_chat_list_);
}

void Td::on_request(uint64 id, td_api::searchChatMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(SearchChatMessagesRequest, request.chat_id_, std::move(request.query_), request.sender_user_id_,
                 request.from_message_id_, request.offset_, request.limit_, std::move(request.filter_));
}

void Td::on_request(uint64 id, td_api::searchSecretMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(OfflineSearchMessagesRequest, request.chat_id_, std::move(request.query_), request.from_search_id_,
                 request.limit_, std::move(request.filter_));
}

void Td::on_request(uint64 id, td_api::searchMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(SearchMessagesRequest, std::move(request.query_), request.offset_date_, request.offset_chat_id_,
                 request.offset_message_id_, request.limit_);
}

void Td::on_request(uint64 id, td_api::searchCallMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(SearchCallMessagesRequest, request.from_message_id_, request.limit_, request.only_missed_);
}

void Td::on_request(uint64 id, const td_api::searchChatRecentLocationMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(SearchChatRecentLocationMessagesRequest, request.chat_id_, request.limit_);
}

void Td::on_request(uint64 id, const td_api::getActiveLiveLocationMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetActiveLiveLocationMessagesRequest);
}

void Td::on_request(uint64 id, const td_api::getChatMessageByDate &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetChatMessageByDateRequest, request.chat_id_, request.date_);
}

void Td::on_request(uint64 id, const td_api::deleteMessages &request) {
  CHECK_AUTH();
  CREATE_REQUEST(DeleteMessagesRequest, request.chat_id_, request.message_ids_, request.revoke_);
}

void Td::on_request(uint64 id, const td_api::deleteChatMessagesFromUser &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(DeleteChatMessagesFromUserRequest, request.chat_id_, request.user_id_);
}

void Td::on_request(uint64 id, const td_api::readAllChatMentions &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ReadAllChatMentionsRequest, request.chat_id_);
}

void Td::on_request(uint64 id, td_api::sendMessage &request) {
  CHECK_AUTH();

  DialogId dialog_id(request.chat_id_);
  auto r_new_message_id = messages_manager_->send_message(
      dialog_id, MessageId(request.reply_to_message_id_), request.disable_notification_, request.from_background_,
      std::move(request.reply_markup_), std::move(request.input_message_content_));
  if (r_new_message_id.is_error()) {
    return send_closure(actor_id(this), &Td::send_error, id, r_new_message_id.move_as_error());
  }

  CHECK(r_new_message_id.ok().is_valid());
  send_closure(actor_id(this), &Td::send_result, id,
               messages_manager_->get_message_object({dialog_id, r_new_message_id.ok()}));
}

void Td::on_request(uint64 id, td_api::sendMessageAlbum &request) {
  CHECK_AUTH();

  DialogId dialog_id(request.chat_id_);
  auto r_message_ids = messages_manager_->send_message_group(dialog_id, MessageId(request.reply_to_message_id_),
                                                             request.disable_notification_, request.from_background_,
                                                             std::move(request.input_message_contents_));
  if (r_message_ids.is_error()) {
    return send_closure(actor_id(this), &Td::send_error, id, r_message_ids.move_as_error());
  }

  send_closure(actor_id(this), &Td::send_result, id,
               messages_manager_->get_messages_object(-1, dialog_id, r_message_ids.ok()));
}

void Td::on_request(uint64 id, td_api::sendBotStartMessage &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.parameter_);

  DialogId dialog_id(request.chat_id_);
  auto r_new_message_id =
      messages_manager_->send_bot_start_message(UserId(request.bot_user_id_), dialog_id, request.parameter_);
  if (r_new_message_id.is_error()) {
    return send_closure(actor_id(this), &Td::send_error, id, r_new_message_id.move_as_error());
  }

  CHECK(r_new_message_id.ok().is_valid());
  send_closure(actor_id(this), &Td::send_result, id,
               messages_manager_->get_message_object({dialog_id, r_new_message_id.ok()}));
}

void Td::on_request(uint64 id, td_api::sendInlineQueryResultMessage &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.result_id_);

  DialogId dialog_id(request.chat_id_);
  auto r_new_message_id = messages_manager_->send_inline_query_result_message(
      dialog_id, MessageId(request.reply_to_message_id_), request.disable_notification_, request.from_background_,
      request.query_id_, request.result_id_);
  if (r_new_message_id.is_error()) {
    return send_closure(actor_id(this), &Td::send_error, id, r_new_message_id.move_as_error());
  }

  CHECK(r_new_message_id.ok().is_valid());
  send_closure(actor_id(this), &Td::send_result, id,
               messages_manager_->get_message_object({dialog_id, r_new_message_id.ok()}));
}

void Td::on_request(uint64 id, const td_api::sendChatSetTtlMessage &request) {
  CHECK_AUTH();

  DialogId dialog_id(request.chat_id_);
  auto r_new_message_id = messages_manager_->send_dialog_set_ttl_message(dialog_id, request.ttl_);
  if (r_new_message_id.is_error()) {
    return send_closure(actor_id(this), &Td::send_error, id, r_new_message_id.move_as_error());
  }

  CHECK(r_new_message_id.ok().is_valid());
  send_closure(actor_id(this), &Td::send_result, id,
               messages_manager_->get_message_object({dialog_id, r_new_message_id.ok()}));
}

void Td::on_request(uint64 id, td_api::editMessageText &request) {
  CHECK_AUTH();
  CREATE_REQUEST(EditMessageTextRequest, request.chat_id_, request.message_id_, std::move(request.reply_markup_),
                 std::move(request.input_message_content_));
}

void Td::on_request(uint64 id, td_api::editMessageLiveLocation &request) {
  CHECK_AUTH();
  CREATE_REQUEST(EditMessageLiveLocationRequest, request.chat_id_, request.message_id_,
                 std::move(request.reply_markup_), std::move(request.location_));
}

void Td::on_request(uint64 id, td_api::editMessageCaption &request) {
  CHECK_AUTH();
  CREATE_REQUEST(EditMessageCaptionRequest, request.chat_id_, request.message_id_, std::move(request.reply_markup_),
                 std::move(request.caption_));
}

void Td::on_request(uint64 id, td_api::editMessageReplyMarkup &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CREATE_REQUEST(EditMessageReplyMarkupRequest, request.chat_id_, request.message_id_,
                 std::move(request.reply_markup_));
}

void Td::on_request(uint64 id, td_api::editInlineMessageText &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.inline_message_id_);
  CREATE_REQUEST(EditInlineMessageTextRequest, std::move(request.inline_message_id_), std::move(request.reply_markup_),
                 std::move(request.input_message_content_));
}

void Td::on_request(uint64 id, td_api::editInlineMessageLiveLocation &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.inline_message_id_);
  CREATE_REQUEST(EditInlineMessageLiveLocationRequest, std::move(request.inline_message_id_),
                 std::move(request.reply_markup_), std::move(request.location_));
}

void Td::on_request(uint64 id, td_api::editInlineMessageCaption &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.inline_message_id_);
  CREATE_REQUEST(EditInlineMessageCaptionRequest, std::move(request.inline_message_id_),
                 std::move(request.reply_markup_), std::move(request.caption_));
}

void Td::on_request(uint64 id, td_api::editInlineMessageReplyMarkup &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.inline_message_id_);
  CREATE_REQUEST(EditInlineMessageReplyMarkupRequest, std::move(request.inline_message_id_),
                 std::move(request.reply_markup_));
}

void Td::on_request(uint64 id, td_api::setGameScore &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CREATE_REQUEST(SetGameScoreRequest, request.chat_id_, request.message_id_, request.edit_message_, request.user_id_,
                 request.score_, request.force_);
}

void Td::on_request(uint64 id, td_api::setInlineGameScore &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.inline_message_id_);
  CREATE_REQUEST(SetInlineGameScoreRequest, std::move(request.inline_message_id_), request.edit_message_,
                 request.user_id_, request.score_, request.force_);
}

void Td::on_request(uint64 id, td_api::getGameHighScores &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CREATE_REQUEST(GetGameHighScoresRequest, request.chat_id_, request.message_id_, request.user_id_);
}

void Td::on_request(uint64 id, td_api::getInlineGameHighScores &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.inline_message_id_);
  CREATE_REQUEST(GetInlineGameHighScoresRequest, std::move(request.inline_message_id_), request.user_id_);
}

void Td::on_request(uint64 id, const td_api::deleteChatReplyMarkup &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(
      id, messages_manager_->delete_dialog_reply_markup(DialogId(request.chat_id_), MessageId(request.message_id_)));
}

void Td::on_request(uint64 id, td_api::sendChatAction &request) {
  CHECK_AUTH();
  CREATE_REQUEST(SendChatActionRequest, request.chat_id_, std::move(request.action_));
}

void Td::on_request(uint64 id, td_api::sendChatScreenshotTakenNotification &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, messages_manager_->send_screenshot_taken_notification_message(DialogId(request.chat_id_)));
}

void Td::on_request(uint64 id, const td_api::forwardMessages &request) {
  CHECK_AUTH();

  DialogId dialog_id(request.chat_id_);
  auto r_message_ids = messages_manager_->forward_messages(
      dialog_id, DialogId(request.from_chat_id_), MessagesManager::get_message_ids(request.message_ids_),
      request.disable_notification_, request.from_background_, false, request.as_album_);
  if (r_message_ids.is_error()) {
    return send_closure(actor_id(this), &Td::send_error, id, r_message_ids.move_as_error());
  }

  send_closure(actor_id(this), &Td::send_result, id,
               messages_manager_->get_messages_object(-1, dialog_id, r_message_ids.ok()));
}

void Td::on_request(uint64 id, td_api::getWebPagePreview &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.message_text_);
  CREATE_REQUEST(GetWebPagePreviewRequest, std::move(request.message_text_));
}

void Td::on_request(uint64 id, td_api::getWebPageInstantView &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.url_);
  CREATE_REQUEST(GetWebPageInstantViewRequest, std::move(request.url_), request.force_full_);
}

void Td::on_request(uint64 id, const td_api::createPrivateChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST(CreateChatRequest, DialogId(UserId(request.user_id_)), request.force_);
}

void Td::on_request(uint64 id, const td_api::createBasicGroupChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST(CreateChatRequest, DialogId(ChatId(request.basic_group_id_)), request.force_);
}

void Td::on_request(uint64 id, const td_api::createSupergroupChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST(CreateChatRequest, DialogId(ChannelId(request.supergroup_id_)), request.force_);
}

void Td::on_request(uint64 id, td_api::createSecretChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST(CreateChatRequest, DialogId(SecretChatId(request.secret_chat_id_)), true);
}

void Td::on_request(uint64 id, td_api::createNewBasicGroupChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.title_);
  CREATE_REQUEST(CreateNewGroupChatRequest, request.user_ids_, std::move(request.title_));
}

void Td::on_request(uint64 id, td_api::createNewSupergroupChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.title_);
  CLEAN_INPUT_STRING(request.description_);
  CREATE_REQUEST(CreateNewSupergroupChatRequest, std::move(request.title_), !request.is_channel_,
                 std::move(request.description_));
}
void Td::on_request(uint64 id, td_api::createNewSecretChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST(CreateNewSecretChatRequest, request.user_id_);
}

void Td::on_request(uint64 id, td_api::createCall &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<CallId> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(result.ok().as_td_api());
    }
  });

  if (!request.protocol_) {
    return query_promise.set_error(Status::Error(5, "CallProtocol must not be empty"));
  }

  UserId user_id(request.user_id_);
  auto input_user = contacts_manager_->get_input_user(user_id);
  if (input_user == nullptr) {
    return query_promise.set_error(Status::Error(6, "User not found"));
  }

  if (!G()->shared_config().get_option_boolean("calls_enabled")) {
    return query_promise.set_error(Status::Error(7, "Calls are not enabled for the current user"));
  }

  send_closure(G()->call_manager(), &CallManager::create_call, user_id, std::move(input_user),
               CallProtocol::from_td_api(*request.protocol_), std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::discardCall &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  send_closure(G()->call_manager(), &CallManager::discard_call, CallId(request.call_id_), request.is_disconnected_,
               request.duration_, request.connection_id_, std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::acceptCall &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  if (!request.protocol_) {
    return query_promise.set_error(Status::Error(5, "Call protocol must not be empty"));
  }
  send_closure(G()->call_manager(), &CallManager::accept_call, CallId(request.call_id_),
               CallProtocol::from_td_api(*request.protocol_), std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::sendCallRating &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.comment_);
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  send_closure(G()->call_manager(), &CallManager::rate_call, CallId(request.call_id_), request.rating_,
               std::move(request.comment_), std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::sendCallDebugInformation &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.debug_information_);
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  send_closure(G()->call_manager(), &CallManager::send_call_debug_information, CallId(request.call_id_),
               std::move(request.debug_information_), std::move(query_promise));
}

void Td::on_request(uint64 id, const td_api::upgradeBasicGroupChatToSupergroupChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(UpgradeGroupChatToSupergroupChatRequest, request.chat_id_);
}

void Td::on_request(uint64 id, td_api::setChatTitle &request) {
  CHECK_AUTH();
  CLEAN_INPUT_STRING(request.title_);
  CREATE_REQUEST(SetChatTitleRequest, request.chat_id_, std::move(request.title_));
}

void Td::on_request(uint64 id, td_api::setChatPhoto &request) {
  CHECK_AUTH();
  CREATE_REQUEST(SetChatPhotoRequest, request.chat_id_, std::move(request.photo_));
}

void Td::on_request(uint64 id, td_api::setChatDraftMessage &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(
      id, messages_manager_->set_dialog_draft_message(DialogId(request.chat_id_), std::move(request.draft_message_)));
}

void Td::on_request(uint64 id, const td_api::toggleChatIsPinned &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, messages_manager_->toggle_dialog_is_pinned(DialogId(request.chat_id_), request.is_pinned_));
}

void Td::on_request(uint64 id, const td_api::setPinnedChats &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, messages_manager_->set_pinned_dialogs(
                          transform(request.chat_ids_, [](int64 chat_id) { return DialogId(chat_id); })));
}

void Td::on_request(uint64 id, td_api::setChatClientData &request) {
  CHECK_AUTH();
  answer_ok_query(
      id, messages_manager_->set_dialog_client_data(DialogId(request.chat_id_), std::move(request.client_data_)));
}

void Td::on_request(uint64 id, const td_api::addChatMember &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(AddChatMemberRequest, request.chat_id_, request.user_id_, request.forward_limit_);
}

void Td::on_request(uint64 id, const td_api::addChatMembers &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(AddChatMembersRequest, request.chat_id_, request.user_ids_);
}

void Td::on_request(uint64 id, td_api::setChatMemberStatus &request) {
  CHECK_AUTH();
  CREATE_REQUEST(SetChatMemberStatusRequest, request.chat_id_, request.user_id_, std::move(request.status_));
}

void Td::on_request(uint64 id, const td_api::getChatMember &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetChatMemberRequest, request.chat_id_, request.user_id_);
}

void Td::on_request(uint64 id, td_api::searchChatMembers &request) {
  CHECK_AUTH();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(SearchChatMembersRequest, request.chat_id_, std::move(request.query_), request.limit_);
}

void Td::on_request(uint64 id, td_api::getChatAdministrators &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetChatAdministratorsRequest, request.chat_id_);
}

void Td::on_request(uint64 id, const td_api::generateChatInviteLink &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GenerateChatInviteLinkRequest, request.chat_id_);
}

void Td::on_request(uint64 id, td_api::checkChatInviteLink &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.invite_link_);
  CREATE_REQUEST(CheckChatInviteLinkRequest, request.invite_link_);
}

void Td::on_request(uint64 id, td_api::joinChatByInviteLink &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.invite_link_);
  CREATE_REQUEST(JoinChatByInviteLinkRequest, request.invite_link_);
}

void Td::on_request(uint64 id, td_api::getChatEventLog &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(GetChatEventLogRequest, request.chat_id_, std::move(request.query_), request.from_event_id_,
                 request.limit_, std::move(request.filters_), std::move(request.user_ids_));
}

void Td::on_request(uint64 id, const td_api::downloadFile &request) {
  CHECK_AUTH();

  auto priority = request.priority_;
  if (!(1 <= priority && priority <= 32)) {
    return send_error_raw(id, 5, "Download priority must be in [1;32] range");
  }
  file_manager_->download(FileId(request.file_id_), download_file_callback_, priority);

  auto file = file_manager_->get_file_object(FileId(request.file_id_), false);
  if (file->id_ == 0) {
    return send_error_raw(id, 400, "Invalid file id");
  }

  send_closure(actor_id(this), &Td::send_result, id, std::move(file));
}

void Td::on_request(uint64 id, const td_api::cancelDownloadFile &request) {
  CHECK_AUTH();

  file_manager_->download(FileId(request.file_id_), nullptr, request.only_if_pending_ ? -1 : 0);

  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::uploadFile &request) {
  CHECK_AUTH();

  auto priority = request.priority_;
  if (!(1 <= priority && priority <= 32)) {
    return send_error_raw(id, 5, "Upload priority must be in [1;32] range");
  }

  auto file_type = request.file_type_ == nullptr ? FileType::Temp : from_td_api(*request.file_type_);
  bool is_secret = file_type == FileType::Encrypted || file_type == FileType::EncryptedThumbnail;
  auto r_file_id = file_manager_->get_input_file_id(file_type, request.file_, DialogId(), false, is_secret, true);
  if (r_file_id.is_error()) {
    return send_error_raw(id, 400, r_file_id.error().message());
  }
  auto file_id = r_file_id.ok();
  auto upload_file_id = file_manager_->dup_file_id(file_id);

  file_manager_->upload(upload_file_id, upload_file_callback_, priority, 0);

  send_closure(actor_id(this), &Td::send_result, id, file_manager_->get_file_object(upload_file_id, false));
}

void Td::on_request(uint64 id, const td_api::cancelUploadFile &request) {
  CHECK_AUTH();

  file_manager_->upload(FileId(request.file_id_), nullptr, 0, 0);

  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, const td_api::setFileGenerationProgress &request) {
  CHECK_AUTH();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  send_closure(file_manager_actor_, &FileManager::external_file_generate_progress, request.generation_id_,
               request.expected_size_, request.local_prefix_size_, std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::finishFileGeneration &request) {
  CHECK_AUTH();
  Status status;
  if (request.error_ != nullptr) {
    CLEAN_INPUT_STRING(request.error_->message_);
    status = Status::Error(request.error_->code_, request.error_->message_);
  }
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  send_closure(file_manager_actor_, &FileManager::external_file_generate_finish, request.generation_id_,
               std::move(status), std::move(query_promise));
}

void Td::on_request(uint64 id, const td_api::deleteFile &request) {
  CHECK_AUTH();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });

  send_closure(file_manager_actor_, &FileManager::delete_file, FileId(request.file_id_), std::move(query_promise),
               "td_api::deleteFile");
}

void Td::on_request(uint64 id, const td_api::blockUser &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, contacts_manager_->block_user(UserId(request.user_id_)));
}

void Td::on_request(uint64 id, const td_api::unblockUser &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  answer_ok_query(id, contacts_manager_->unblock_user(UserId(request.user_id_)));
}

void Td::on_request(uint64 id, const td_api::getBlockedUsers &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetBlockedUsersRequest, request.offset_, request.limit_);
}

void Td::on_request(uint64 id, td_api::importContacts &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  for (auto &contact : request.contacts_) {
    if (contact == nullptr) {
      return send_error_raw(id, 5, "Contact must not be empty");
    }
    CLEAN_INPUT_STRING(contact->phone_number_);
    CLEAN_INPUT_STRING(contact->first_name_);
    CLEAN_INPUT_STRING(contact->last_name_);
  }
  CREATE_REQUEST(ImportContactsRequest, std::move(request.contacts_));
}

void Td::on_request(uint64 id, td_api::searchContacts &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CREATE_REQUEST(SearchContactsRequest, request.query_, request.limit_);
}

void Td::on_request(uint64 id, td_api::removeContacts &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(RemoveContactsRequest, std::move(request.user_ids_));
}

void Td::on_request(uint64 id, const td_api::getImportedContactCount &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetImportedContactCountRequest);
}

void Td::on_request(uint64 id, td_api::changeImportedContacts &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  for (auto &contact : request.contacts_) {
    if (contact == nullptr) {
      return send_error_raw(id, 5, "Contact must not be empty");
    }
    CLEAN_INPUT_STRING(contact->phone_number_);
    CLEAN_INPUT_STRING(contact->first_name_);
    CLEAN_INPUT_STRING(contact->last_name_);
  }
  CREATE_REQUEST(ChangeImportedContactsRequest, std::move(request.contacts_));
}

void Td::on_request(uint64 id, const td_api::clearImportedContacts &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(ClearImportedContactsRequest);
}

void Td::on_request(uint64 id, const td_api::getRecentInlineBots &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetRecentInlineBotsRequest);
}

void Td::on_request(uint64 id, td_api::setName &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.first_name_);
  CLEAN_INPUT_STRING(request.last_name_);
  CREATE_REQUEST(SetNameRequest, std::move(request.first_name_), std::move(request.last_name_));
}

void Td::on_request(uint64 id, td_api::setBio &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.bio_);
  CREATE_REQUEST(SetBioRequest, std::move(request.bio_));
}

void Td::on_request(uint64 id, td_api::setUsername &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.username_);
  CREATE_REQUEST(SetUsernameRequest, std::move(request.username_));
}

void Td::on_request(uint64 id, td_api::setProfilePhoto &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(SetProfilePhotoRequest, std::move(request.photo_));
}

void Td::on_request(uint64 id, const td_api::deleteProfilePhoto &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(DeleteProfilePhotoRequest, request.profile_photo_id_);
}

void Td::on_request(uint64 id, const td_api::getUserProfilePhotos &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetUserProfilePhotosRequest, request.user_id_, request.offset_, request.limit_);
}

void Td::on_request(uint64 id, const td_api::toggleBasicGroupAdministrators &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ToggleGroupAdministratorsRequest, request.basic_group_id_, request.everyone_is_administrator_);
}

void Td::on_request(uint64 id, td_api::setSupergroupUsername &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.username_);
  CREATE_REQUEST(SetSupergroupUsernameRequest, request.supergroup_id_, std::move(request.username_));
}

void Td::on_request(uint64 id, const td_api::setSupergroupStickerSet &request) {
  CHECK_AUTH();
  CREATE_REQUEST(SetSupergroupStickerSetRequest, request.supergroup_id_, request.sticker_set_id_);
}

void Td::on_request(uint64 id, const td_api::toggleSupergroupInvites &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ToggleSupergroupInvitesRequest, request.supergroup_id_, request.anyone_can_invite_);
}

void Td::on_request(uint64 id, const td_api::toggleSupergroupSignMessages &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ToggleSupergroupSignMessagesRequest, request.supergroup_id_, request.sign_messages_);
}

void Td::on_request(uint64 id, const td_api::toggleSupergroupIsAllHistoryAvailable &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ToggleSupergroupIsAllHistoryAvailableRequest, request.supergroup_id_,
                 request.is_all_history_available_);
}

void Td::on_request(uint64 id, td_api::setSupergroupDescription &request) {
  CHECK_AUTH();
  CLEAN_INPUT_STRING(request.description_);
  CREATE_REQUEST(SetSupergroupDescriptionRequest, request.supergroup_id_, std::move(request.description_));
}

void Td::on_request(uint64 id, const td_api::pinSupergroupMessage &request) {
  CHECK_AUTH();
  CREATE_REQUEST(PinSupergroupMessageRequest, request.supergroup_id_, request.message_id_,
                 request.disable_notification_);
}

void Td::on_request(uint64 id, const td_api::unpinSupergroupMessage &request) {
  CHECK_AUTH();
  CREATE_REQUEST(UnpinSupergroupMessageRequest, request.supergroup_id_);
}

void Td::on_request(uint64 id, const td_api::reportSupergroupSpam &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ReportSupergroupSpamRequest, request.supergroup_id_, request.user_id_, request.message_ids_);
}

void Td::on_request(uint64 id, td_api::getSupergroupMembers &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetSupergroupMembersRequest, request.supergroup_id_, std::move(request.filter_), request.offset_,
                 request.limit_);
}

void Td::on_request(uint64 id, const td_api::deleteSupergroup &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(DeleteSupergroupRequest, request.supergroup_id_);
}

void Td::on_request(uint64 id, td_api::closeSecretChat &request) {
  CHECK_AUTH();
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  send_closure(secret_chats_manager_, &SecretChatsManager::cancel_chat, SecretChatId(request.secret_chat_id_),
               std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::getStickers &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.emoji_);
  CREATE_REQUEST(GetStickersRequest, std::move(request.emoji_), request.limit_);
}

void Td::on_request(uint64 id, const td_api::getInstalledStickerSets &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetInstalledStickerSetsRequest, request.is_masks_);
}

void Td::on_request(uint64 id, const td_api::getArchivedStickerSets &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetArchivedStickerSetsRequest, request.is_masks_, request.offset_sticker_set_id_, request.limit_);
}

void Td::on_request(uint64 id, const td_api::getTrendingStickerSets &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetTrendingStickerSetsRequest);
}

void Td::on_request(uint64 id, const td_api::getAttachedStickerSets &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetAttachedStickerSetsRequest, request.file_id_);
}

void Td::on_request(uint64 id, const td_api::getStickerSet &request) {
  CHECK_AUTH();
  CREATE_REQUEST(GetStickerSetRequest, request.set_id_);
}

void Td::on_request(uint64 id, td_api::searchStickerSet &request) {
  CHECK_AUTH();
  CLEAN_INPUT_STRING(request.name_);
  CREATE_REQUEST(SearchStickerSetRequest, std::move(request.name_));
}

void Td::on_request(uint64 id, const td_api::changeStickerSet &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ChangeStickerSetRequest, request.set_id_, request.is_installed_, request.is_archived_);
}

void Td::on_request(uint64 id, const td_api::viewTrendingStickerSets &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  stickers_manager_->view_featured_sticker_sets(request.sticker_set_ids_);
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::reorderInstalledStickerSets &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ReorderInstalledStickerSetsRequest, request.is_masks_, std::move(request.sticker_set_ids_));
}

void Td::on_request(uint64 id, td_api::uploadStickerFile &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CREATE_REQUEST(UploadStickerFileRequest, request.user_id_, std::move(request.png_sticker_));
}

void Td::on_request(uint64 id, td_api::createNewStickerSet &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.title_);
  CLEAN_INPUT_STRING(request.name_);
  CREATE_REQUEST(CreateNewStickerSetRequest, request.user_id_, std::move(request.title_), std::move(request.name_),
                 request.is_masks_, std::move(request.stickers_));
}

void Td::on_request(uint64 id, td_api::addStickerToSet &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.name_);
  CREATE_REQUEST(AddStickerToSetRequest, request.user_id_, std::move(request.name_), std::move(request.sticker_));
}

void Td::on_request(uint64 id, td_api::setStickerPositionInSet &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CREATE_REQUEST(SetStickerPositionInSetRequest, std::move(request.sticker_), request.position_);
}

void Td::on_request(uint64 id, td_api::removeStickerFromSet &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CREATE_REQUEST(RemoveStickerFromSetRequest, std::move(request.sticker_));
}

void Td::on_request(uint64 id, const td_api::getRecentStickers &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetRecentStickersRequest, request.is_attached_);
}

void Td::on_request(uint64 id, td_api::addRecentSticker &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(AddRecentStickerRequest, request.is_attached_, std::move(request.sticker_));
}

void Td::on_request(uint64 id, td_api::removeRecentSticker &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(RemoveRecentStickerRequest, request.is_attached_, std::move(request.sticker_));
}

void Td::on_request(uint64 id, td_api::clearRecentStickers &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ClearRecentStickersRequest, request.is_attached_);
}

void Td::on_request(uint64 id, const td_api::getFavoriteStickers &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetFavoriteStickersRequest);
}

void Td::on_request(uint64 id, td_api::addFavoriteSticker &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(AddFavoriteStickerRequest, std::move(request.sticker_));
}

void Td::on_request(uint64 id, td_api::removeFavoriteSticker &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(RemoveFavoriteStickerRequest, std::move(request.sticker_));
}

void Td::on_request(uint64 id, td_api::getStickerEmojis &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetStickerEmojisRequest, std::move(request.sticker_));
}

void Td::on_request(uint64 id, const td_api::getSavedAnimations &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetSavedAnimationsRequest);
}

void Td::on_request(uint64 id, td_api::addSavedAnimation &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(AddSavedAnimationRequest, std::move(request.animation_));
}

void Td::on_request(uint64 id, td_api::removeSavedAnimation &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(RemoveSavedAnimationRequest, std::move(request.animation_));
}

void Td::on_request(uint64 id, const td_api::getNotificationSettings &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetNotificationSettingsRequest, messages_manager_->get_notification_settings_scope(request.scope_));
}

void Td::on_request(uint64 id, const td_api::getChatReportSpamState &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetChatReportSpamStateRequest, request.chat_id_);
}

void Td::on_request(uint64 id, const td_api::changeChatReportSpamState &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ChangeChatReportSpamStateRequest, request.chat_id_, request.is_spam_chat_);
}

void Td::on_request(uint64 id, td_api::reportChat &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ReportChatRequest, request.chat_id_, std::move(request.reason_));
}

void Td::on_request(uint64 id, td_api::setNotificationSettings &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.notification_settings_->sound_);
  answer_ok_query(id, messages_manager_->set_notification_settings(
                          messages_manager_->get_notification_settings_scope(request.scope_),
                          std::move(request.notification_settings_)));
}

void Td::on_request(uint64 id, const td_api::resetAllNotificationSettings &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  messages_manager_->reset_all_notification_settings();
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::getOption &request) {
  CLEAN_INPUT_STRING(request.name_);

  tl_object_ptr<td_api::OptionValue> option_value;
  switch (request.name_[0]) {
    case 'o':
      if (request.name_ == "online") {
        option_value = make_tl_object<td_api::optionValueBoolean>(is_online_);
      }
      break;
    case 'v':
      if (request.name_ == "version") {
        option_value = make_tl_object<td_api::optionValueString>(tdlib_version);
      }
      break;
  }
  if (option_value == nullptr) {
    option_value = G()->shared_config().get_option_value(request.name_);
  }
  send_closure(actor_id(this), &Td::send_result, id, std::move(option_value));
}

void Td::on_request(uint64 id, td_api::setOption &request) {
  CLEAN_INPUT_STRING(request.name_);
  int32 value_constructor_id = request.value_ == nullptr ? td_api::optionValueEmpty::ID : request.value_->get_id();

  auto set_integer_option = [&](Slice name, int32 min = 0, int32 max = std::numeric_limits<int32>::max()) {
    if (request.name_ == name) {
      if (value_constructor_id != td_api::optionValueInteger::ID &&
          value_constructor_id != td_api::optionValueEmpty::ID) {
        send_error_raw(id, 3, PSLICE() << "Option \"" << name << "\" must have integer value");
        return true;
      }
      if (value_constructor_id == td_api::optionValueEmpty::ID) {
        G()->shared_config().set_option_empty(name);
      } else {
        int32 value = static_cast<td_api::optionValueInteger *>(request.value_.get())->value_;
        if (value < min || value > max) {
          send_error_raw(id, 3,
                         PSLICE() << "Option's \"" << name << "\" value " << value << " is outside of a valid range ["
                                  << min << ", " << max << "]");
          return true;
        }
        G()->shared_config().set_option_integer(name, std::max(std::min(value, max), min));
      }
      send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
      return true;
    }
    return false;
  };

  auto set_boolean_option = [&](Slice name) {
    if (request.name_ == name) {
      if (value_constructor_id != td_api::optionValueBoolean::ID &&
          value_constructor_id != td_api::optionValueEmpty::ID) {
        send_error_raw(id, 3, PSLICE() << "Option \"" << name << "\" must have boolean value");
        return true;
      }
      if (value_constructor_id == td_api::optionValueEmpty::ID) {
        G()->shared_config().set_option_empty(name);
      } else {
        bool value = static_cast<td_api::optionValueBoolean *>(request.value_.get())->value_;
        G()->shared_config().set_option_boolean(name, value);
      }
      send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
      return true;
    }
    return false;
  };

  switch (request.name_[0]) {
    case 'd':
      if (set_boolean_option("disable_contact_registered_notifications")) {
        return;
      }
      break;
    case 'o':
      if (request.name_ == "online") {
        if (value_constructor_id != td_api::optionValueBoolean::ID &&
            value_constructor_id != td_api::optionValueEmpty::ID) {
          return send_error_raw(id, 3, "Option \"online\" must have boolean value");
        }
        bool is_online = value_constructor_id == td_api::optionValueEmpty::ID ||
                         static_cast<const td_api::optionValueBoolean *>(request.value_.get())->value_;
        if (!auth_manager_->is_bot()) {
          send_closure(G()->state_manager(), &StateManager::on_online, is_online);
        }
        if (is_online != is_online_) {
          is_online_ = is_online;
          on_online_updated(true, true);
        }
        return send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
      }
      break;
    case 's':
      if (set_integer_option("session_count", 0, 50)) {
        return;
      }
      if (set_integer_option("storage_max_files_size")) {
        return;
      }
      if (set_integer_option("storage_max_time_from_last_access")) {
        return;
      }
      if (set_integer_option("storage_max_file_count")) {
        return;
      }
      if (set_integer_option("storage_immunity_delay")) {
        return;
      }
      break;
    case 'X':
    case 'x': {
      if (request.name_.size() > 255) {
        return send_error_raw(id, 3, "Option name is too long");
      }
      switch (value_constructor_id) {
        case td_api::optionValueBoolean::ID:
          G()->shared_config().set_option_boolean(
              request.name_, static_cast<const td_api::optionValueBoolean *>(request.value_.get())->value_);
          break;
        case td_api::optionValueEmpty::ID:
          G()->shared_config().set_option_empty(request.name_);
          break;
        case td_api::optionValueInteger::ID:
          G()->shared_config().set_option_integer(
              request.name_, static_cast<const td_api::optionValueInteger *>(request.value_.get())->value_);
          break;
        case td_api::optionValueString::ID:
          G()->shared_config().set_option_string(
              request.name_, static_cast<const td_api::optionValueString *>(request.value_.get())->value_);
          break;
        default:
          UNREACHABLE();
      }
      return send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
    }
    case 'u':
      if (set_boolean_option("use_pfs")) {
        return;
      }
      if (set_boolean_option("use_quick_ack")) {
        return;
      }
      if (set_boolean_option("use_storage_optimizer")) {
        return;
      }
      break;
  }

  return send_error_raw(id, 3, "Option can't be set");
}

void Td::on_request(uint64 id, td_api::getInlineQueryResults &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.query_);
  CLEAN_INPUT_STRING(request.offset_);
  CREATE_REQUEST(GetInlineQueryResultsRequest, request.bot_user_id_, request.chat_id_, request.user_location_,
                 std::move(request.query_), std::move(request.offset_));
}

void Td::on_request(uint64 id, td_api::answerInlineQuery &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.next_offset_);
  CLEAN_INPUT_STRING(request.switch_pm_text_);
  CLEAN_INPUT_STRING(request.switch_pm_parameter_);
  CREATE_REQUEST(AnswerInlineQueryRequest, request.inline_query_id_, request.is_personal_, std::move(request.results_),
                 request.cache_time_, std::move(request.next_offset_), std::move(request.switch_pm_text_),
                 std::move(request.switch_pm_parameter_));
}

void Td::on_request(uint64 id, td_api::getCallbackQueryAnswer &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetCallbackQueryAnswerRequest, request.chat_id_, request.message_id_, std::move(request.payload_));
}

void Td::on_request(uint64 id, td_api::answerCallbackQuery &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.text_);
  CLEAN_INPUT_STRING(request.url_);
  CREATE_REQUEST(AnswerCallbackQueryRequest, request.callback_query_id_, std::move(request.text_), request.show_alert_,
                 std::move(request.url_), request.cache_time_);
}

void Td::on_request(uint64 id, td_api::answerShippingQuery &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.error_message_);
  CREATE_REQUEST(AnswerShippingQueryRequest, request.shipping_query_id_, std::move(request.shipping_options_),
                 std::move(request.error_message_));
}

void Td::on_request(uint64 id, td_api::answerPreCheckoutQuery &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.error_message_);
  CREATE_REQUEST(AnswerPreCheckoutQueryRequest, request.pre_checkout_query_id_, std::move(request.error_message_));
}

void Td::on_request(uint64 id, const td_api::getPaymentForm &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetPaymentFormRequest, request.chat_id_, request.message_id_);
}

void Td::on_request(uint64 id, td_api::validateOrderInfo &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(ValidateOrderInfoRequest, request.chat_id_, request.message_id_, std::move(request.order_info_),
                 request.allow_save_);
}

void Td::on_request(uint64 id, td_api::sendPaymentForm &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.order_info_id_);
  CLEAN_INPUT_STRING(request.shipping_option_id_);
  if (request.credentials_ == nullptr) {
    return send_error_raw(id, 400, "Input payments credentials must not be empty");
  }
  CREATE_REQUEST(SendPaymentFormRequest, request.chat_id_, request.message_id_, std::move(request.order_info_id_),
                 std::move(request.shipping_option_id_), std::move(request.credentials_));
}

void Td::on_request(uint64 id, const td_api::getPaymentReceipt &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_REQUEST(GetPaymentReceiptRequest, request.chat_id_, request.message_id_);
}

void Td::on_request(uint64 id, const td_api::getSavedOrderInfo &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetSavedOrderInfoRequest);
}

void Td::on_request(uint64 id, const td_api::deleteSavedOrderInfo &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(DeleteSavedOrderInfoRequest);
}

void Td::on_request(uint64 id, const td_api::deleteSavedCredentials &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(DeleteSavedCredentialsRequest);
}

void Td::on_request(uint64 id, const td_api::getSupportUser &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetSupportUserRequest);
}

void Td::on_request(uint64 id, const td_api::getWallpapers &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetWallpapersRequest);
}

void Td::on_request(uint64 id, td_api::getRecentlyVisitedTMeUrls &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.referrer_);
  CREATE_REQUEST(GetRecentlyVisitedTMeUrlsRequest, std::move(request.referrer_));
}

void Td::on_request(uint64 id, td_api::setBotUpdatesStatus &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.error_message_);
  create_handler<SetBotUpdatesStatusQuery>()->send(request.pending_update_count_, request.error_message_);
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::sendCustomRequest &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.method_);
  CLEAN_INPUT_STRING(request.parameters_);
  CREATE_REQUEST(SendCustomRequestRequest, std::move(request.method_), std::move(request.parameters_));
}

void Td::on_request(uint64 id, td_api::answerCustomQuery &request) {
  CHECK_AUTH();
  CHECK_IS_BOT();
  CLEAN_INPUT_STRING(request.data_);
  CREATE_REQUEST(AnswerCustomQueryRequest, request.custom_query_id_, std::move(request.data_));
}

void Td::on_request(uint64 id, const td_api::setAlarm &request) {
  if (request.seconds_ < 0 || request.seconds_ > 3e9) {
    return send_error_raw(id, 400, "Wrong parameter seconds specified");
  }
  alarm_timeout_.set_timeout_in(static_cast<int64>(id), request.seconds_);
}

void Td::on_request(uint64 id, td_api::searchHashtags &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.prefix_);
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise =
      PromiseCreator::lambda([promise = std::move(promise)](Result<std::vector<string>> result) mutable {
        if (result.is_error()) {
          promise.set_error(result.move_as_error());
        } else {
          promise.set_value(make_tl_object<td_api::hashtags>(result.move_as_ok()));
        }
      });
  send_closure(hashtag_hints_, &HashtagHints::query, std::move(request.prefix_), request.limit_,
               std::move(query_promise));
}

void Td::on_request(uint64 id, td_api::removeRecentHashtag &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CLEAN_INPUT_STRING(request.hashtag_);
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(make_tl_object<td_api::ok>());
    }
  });
  send_closure(hashtag_hints_, &HashtagHints::remove_hashtag, std::move(request.hashtag_), std::move(query_promise));
}

void Td::on_request(uint64 id, const td_api::getInviteText &request) {
  CHECK_AUTH();
  CHECK_IS_USER();
  CREATE_NO_ARGS_REQUEST(GetInviteTextRequest);
}

void Td::on_request(uint64 id, const td_api::getTermsOfService &request) {
  CREATE_NO_ARGS_REQUEST(GetTermsOfServiceRequest);
}

void Td::on_request(uint64 id, const td_api::getProxy &request) {
  CREATE_REQUEST_PROMISE(promise);
  auto query_promise = PromiseCreator::lambda([promise = std::move(promise)](Result<Proxy> result) mutable {
    if (result.is_error()) {
      promise.set_error(result.move_as_error());
    } else {
      promise.set_value(result.move_as_ok().as_td_api());
    }
  });
  send_closure(G()->connection_creator(), &ConnectionCreator::get_proxy, std::move(query_promise));
}

void Td::on_request(uint64 id, const td_api::setProxy &request) {
  CREATE_REQUEST_PROMISE(promise);
  send_closure(G()->connection_creator(), &ConnectionCreator::set_proxy, Proxy::from_td_api(request.proxy_));
  promise.set_value(make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, const td_api::getTextEntities &request) {
  // don't check authorization state
  send_result(id, do_static_request(request));
}

void Td::on_request(uint64 id, td_api::parseTextEntities &request) {
  // don't check authorization state
  send_result(id, do_static_request(request));
}

void Td::on_request(uint64 id, const td_api::getFileMimeType &request) {
  // don't check authorization state
  send_result(id, do_static_request(request));
}

void Td::on_request(uint64 id, const td_api::getFileExtension &request) {
  // don't check authorization state
  send_result(id, do_static_request(request));
}

template <class T>
td_api::object_ptr<td_api::Object> Td::do_static_request(const T &) {
  return create_error_raw(400, "Function can't be executed synchronously");
}

td_api::object_ptr<td_api::Object> Td::do_static_request(const td_api::getTextEntities &request) {
  if (!check_utf8(request.text_)) {
    return create_error_raw(400, "Text must be encoded in UTF-8");
  }
  auto text_entities = find_entities(request.text_, false);
  return make_tl_object<td_api::textEntities>(get_text_entities_object(text_entities));
}

td_api::object_ptr<td_api::Object> Td::do_static_request(td_api::parseTextEntities &request) {
  if (!check_utf8(request.text_)) {
    return create_error_raw(400, "Text must be encoded in UTF-8");
  }
  if (request.parse_mode_ == nullptr) {
    return create_error_raw(400, "Parse mode must be non-empty");
  }

  Result<vector<MessageEntity>> r_entities;
  switch (request.parse_mode_->get_id()) {
    case td_api::textParseModeHTML::ID:
      r_entities = parse_html(request.text_);
      break;
    case td_api::textParseModeMarkdown::ID:
      r_entities = parse_markdown(request.text_);
      break;
    default:
      UNREACHABLE();
      break;
  }
  if (r_entities.is_error()) {
    return create_error_raw(400, PSLICE() << "Can't parse entities: " << r_entities.error().message());
  }

  return make_tl_object<td_api::formattedText>(std::move(request.text_), get_text_entities_object(r_entities.ok()));
}

td_api::object_ptr<td_api::Object> Td::do_static_request(const td_api::getFileMimeType &request) {
  // don't check file name UTF-8 correctness
  return make_tl_object<td_api::text>(MimeType::from_extension(PathView(request.file_name_).extension()));
}

td_api::object_ptr<td_api::Object> Td::do_static_request(const td_api::getFileExtension &request) {
  // don't check MIME type UTF-8 correctness
  return make_tl_object<td_api::text>(MimeType::to_extension(request.mime_type_));
}

// test
void Td::on_request(uint64 id, td_api::testNetwork &request) {
  create_handler<TestQuery>(id)->send();
}

void Td::on_request(uint64 id, td_api::testGetDifference &request) {
  CHECK_AUTH();
  updates_manager_->get_difference("testGetDifference");
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::testUseUpdate &request) {
  send_closure(actor_id(this), &Td::send_result, id, nullptr);
}

void Td::on_request(uint64 id, td_api::testUseError &request) {
  send_closure(actor_id(this), &Td::send_result, id, nullptr);
}

void Td::on_request(uint64 id, td_api::testCallEmpty &request) {
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::ok>());
}

void Td::on_request(uint64 id, td_api::testSquareInt &request) {
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::testInt>(request.x_ * request.x_));
}

void Td::on_request(uint64 id, td_api::testCallString &request) {
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::testString>(std::move(request.x_)));
}

void Td::on_request(uint64 id, td_api::testCallBytes &request) {
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::testBytes>(std::move(request.x_)));
}

void Td::on_request(uint64 id, td_api::testCallVectorInt &request) {
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::testVectorInt>(std::move(request.x_)));
}

void Td::on_request(uint64 id, td_api::testCallVectorIntObject &request) {
  send_closure(actor_id(this), &Td::send_result, id,
               make_tl_object<td_api::testVectorIntObject>(std::move(request.x_)));
}

void Td::on_request(uint64 id, td_api::testCallVectorString &request) {
  send_closure(actor_id(this), &Td::send_result, id, make_tl_object<td_api::testVectorString>(std::move(request.x_)));
}

void Td::on_request(uint64 id, td_api::testCallVectorStringObject &request) {
  send_closure(actor_id(this), &Td::send_result, id,
               make_tl_object<td_api::testVectorStringObject>(std::move(request.x_)));
}

#undef CLEAN_INPUT_STRING
#undef CHECK_AUTH
#undef CHECK_IS_BOT
#undef CHECK_IS_USER
#undef CREATE_NO_ARGS_REQUEST
#undef CREATE_REQUEST
#undef CREATE_REQUEST_PROMISE

constexpr const char *Td::tdlib_version;

}  // namespace td
