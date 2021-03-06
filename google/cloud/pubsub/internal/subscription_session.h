// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_INTERNAL_SUBSCRIPTION_SESSION_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_INTERNAL_SUBSCRIPTION_SESSION_H

#include "google/cloud/pubsub/internal/session_shutdown_manager.h"
#include "google/cloud/pubsub/internal/subscriber_stub.h"
#include "google/cloud/pubsub/internal/watermark_flow_control.h"
#include "google/cloud/pubsub/subscriber_connection.h"
#include "google/cloud/pubsub/version.h"
#include "google/cloud/completion_queue.h"
#include "google/cloud/status_or.h"
#include <deque>
#include <memory>
#include <mutex>

namespace google {
namespace cloud {
namespace pubsub_internal {
inline namespace GOOGLE_CLOUD_CPP_PUBSUB_NS {

class SubscriptionSession
    : public std::enable_shared_from_this<SubscriptionSession> {
 public:
  static auto constexpr kMinimumAckDeadlineSeconds = 10;
  static auto constexpr kMaximumAckDeadlineSeconds = 600;
  static auto constexpr kAckDeadlineSlackSeconds = 2;

  /// The constructor is private to prevent accidents with hared_from_this()
  static std::shared_ptr<SubscriptionSession> Create(
      std::shared_ptr<pubsub_internal::SubscriberStub> s,
      google::cloud::CompletionQueue executor,
      pubsub::SubscriberConnection::SubscribeParams p) {
    return std::shared_ptr<SubscriptionSession>(new SubscriptionSession(
        std::move(s), std::move(executor), std::move(p)));
  }

  /// Start running the subscription, pull the first batch of messages.
  future<Status> Start();

  /// Discard any information about @p ack_id and stop refreshing its deadline.
  void MessageHandled(std::string const& ack_id, std::size_t message_size);

  /// Test-only, speed up the deadline refreshes to keep tests snappy.
  void SetTestRefreshPeriod(std::chrono::milliseconds d) {
    test_refresh_period_ = d;
  }

 private:
  SubscriptionSession(std::shared_ptr<pubsub_internal::SubscriberStub> s,
                      google::cloud::CompletionQueue executor,
                      pubsub::SubscriberConnection::SubscribeParams p)
      : stub_(std::move(s)),
        executor_(std::move(executor)),
        params_(std::move(p)),
        callback_flow_control_(params_.options.concurrency_lwm(),
                               params_.options.concurrency_hwm()),
        test_refresh_period_(0) {}

  /// Stop fetching message batchines and stop updating any deadlines
  void Cancel();

  /// Asynchronously fetch one batch of messages
  void PullOne();

  /// Called when a batch of messages is received.
  void OnPull(StatusOr<google::pubsub::v1::PullResponse> r);

  void HandleQueue() { HandleQueue(std::unique_lock<std::mutex>(mu_)); }

  /// Handle the queue of messages, invoking `callback_` on the next message.
  void HandleQueue(std::unique_lock<std::mutex> lk);

  /// If needed asynchronous update the message leases in the server.
  void RefreshMessageLeases(std::unique_lock<std::mutex> lk);

  /// Invoked when the asynchronous RPC to update message leases completes.
  void OnRefreshMessageLeases(
      google::pubsub::v1::ModifyAckDeadlineRequest const& request,
      std::chrono::system_clock::time_point new_server_deadline);

  /// Start the timer to update ack deadlines.
  void StartRefreshTimer(
      std::unique_lock<std::mutex> lk,
      std::chrono::system_clock::time_point new_server_deadline);

  /// The timer to update ack deadlines has triggered or was cancelled.
  void OnRefreshTimer(bool cancelled);

  template <typename Iterator>
  void NackAll(std::unique_lock<std::mutex> const& lk, Iterator begin,
               Iterator end) {
    google::pubsub::v1::ModifyAckDeadlineRequest request;
    for (auto m = begin; m != end; ++m) {
      request.add_ack_ids(m->ack_id());
    }
    NackAll(lk, std::move(request));
  }
  void NackAll(std::unique_lock<std::mutex> const&,
               google::pubsub::v1::ModifyAckDeadlineRequest request);

  std::shared_ptr<pubsub_internal::SubscriberStub> stub_;
  google::cloud::CompletionQueue executor_;
  pubsub::SubscriberConnection::SubscribeParams params_;
  std::mutex mu_;

  // Used to signal the application when all activity has ceased.
  SessionShutdownManager shutdown_manager_;

  // Concurrency control for callbacks.
  WatermarkFlowControlCountOnly callback_flow_control_;

  // The queue of messages waiting to be delivered to the application callback.
  std::deque<google::pubsub::v1::ReceivedMessage> messages_;

  // A collection of message ack_ids to maintain the message leases.
  struct LeaseStatus {
    std::chrono::system_clock::time_point estimated_server_deadline;
    std::chrono::system_clock::time_point handling_deadline;
  };
  std::map<std::string, LeaseStatus> leases_;

  bool refreshing_leases_ = false;
  future<void> refresh_timer_;

  std::chrono::milliseconds test_refresh_period_;
};

}  // namespace GOOGLE_CLOUD_CPP_PUBSUB_NS
}  // namespace pubsub_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_PUBSUB_INTERNAL_SUBSCRIPTION_SESSION_H
