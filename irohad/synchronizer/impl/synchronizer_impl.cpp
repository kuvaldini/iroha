/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "synchronizer/impl/synchronizer_impl.hpp"

#include <utility>

#include "ametsuchi/block_query_factory.hpp"
#include "ametsuchi/command_executor.hpp"
#include "ametsuchi/mutable_storage.hpp"
#include "common/bind.hpp"
#include "common/result.hpp"
#include "common/visitor.hpp"
#include "interfaces/common_objects/string_view_types.hpp"
#include "interfaces/iroha_internal/block.hpp"
#include "logger/logger.hpp"

using namespace shared_model::interface::types;

namespace iroha {
  namespace synchronizer {

    SynchronizerImpl::SynchronizerImpl(
        std::unique_ptr<iroha::ametsuchi::CommandExecutor> command_executor,
        std::shared_ptr<validation::ChainValidator> validator,
        std::shared_ptr<ametsuchi::MutableFactory> mutable_factory,
        std::shared_ptr<ametsuchi::BlockQueryFactory> block_query_factory,
        std::shared_ptr<network::BlockLoader> block_loader,
        logger::LoggerPtr log)
        : command_executor_(std::move(command_executor)),
          validator_(std::move(validator)),
          mutable_factory_(std::move(mutable_factory)),
          block_query_factory_(std::move(block_query_factory)),
          block_loader_(std::move(block_loader)),
          log_(std::move(log)) {}

    SynchronizationEvent SynchronizerImpl::processOutcome(
        consensus::GateObject object) {
      log_->info("processing consensus outcome");

      auto process_reject = [this](auto outcome_type, const auto &msg) {
        assert(msg.ledger_state->top_block_info.height + 1
               == msg.round.block_round);
        return SynchronizationEvent{outcome_type, msg.round, msg.ledger_state};
      };

      return visit_in_place(
          object,
          [this](const consensus::PairValid &msg) {
            assert(msg.ledger_state->top_block_info.height + 1
                   == msg.round.block_round);
            return this->processNext(msg);
          },
          [this](const consensus::VoteOther &msg) {
            assert(msg.ledger_state->top_block_info.height + 1
                   == msg.round.block_round);
            return this->processDifferent(msg, msg.round.block_round);
          },
          [&](const consensus::ProposalReject &msg) {
            return process_reject(SynchronizationOutcomeType::kReject, msg);
          },
          [&](const consensus::BlockReject &msg) {
            return process_reject(SynchronizationOutcomeType::kReject, msg);
          },
          [&](const consensus::AgreementOnNone &msg) {
            return process_reject(SynchronizationOutcomeType::kNothing, msg);
          },
          [this](const consensus::Future &msg) {
            assert(msg.ledger_state->top_block_info.height + 1
                   < msg.round.block_round);
            // we do not know the ledger state for round n, so we
            // cannot claim that the bunch of votes we got is a
            // commit certificate and hence we do not know if the
            // block n is committed and cannot require its
            // acquisition.
            return this->processDifferent(msg, msg.round.block_round - 1);
          });
    }

    ametsuchi::CommitResult SynchronizerImpl::downloadAndCommitMissingBlocks(
        const shared_model::interface::types::HeightType start_height,
        const shared_model::interface::types::HeightType target_height,
        const PublicKeyCollectionType &public_keys) {
      auto storage_result = getStorage();
      if (iroha::expected::hasError(storage_result)) {
        return std::move(storage_result).assumeError();
      }
      auto storage = std::move(storage_result).assumeValue();
      shared_model::interface::types::HeightType my_height = start_height;

      // TODO andrei 17.10.18 IR-1763 Add delay strategy for loading blocks
      using namespace iroha::expected;
      for (const auto &public_key : public_keys) {
        while (true) {
          bool peer_ok = false;
          log_->debug(
              "trying to download blocks from {} to {} from peer with key {}",
              my_height + 1,
              target_height,
              public_key);
          auto maybe_reader = block_loader_->retrieveBlocks(
              my_height, PublicKeyHexStringView{public_key});

          if (hasError(maybe_reader)) {
            log_->warn(
                "failed to retrieve blocks starting from {} from peer {}: {}",
                my_height,
                public_key,
                maybe_reader.assumeError());
            continue;
          }

          auto block_var = maybe_reader.assumeValue()->read();
          for (auto maybe_block = std::get_if<
                   std::shared_ptr<const shared_model::interface::Block>>(
                   &block_var);
               maybe_block;
               block_var = maybe_reader.assumeValue()->read(),
                    maybe_block = std::get_if<
                        std::shared_ptr<const shared_model::interface::Block>>(
                        &block_var)) {
            if (not(peer_ok =
                        validator_->validateAndApply(*maybe_block, *storage))) {
              break;
            }

            my_height = (*maybe_block)->height();
          }
          if (auto error = std::get_if<std::string>(&block_var)) {
            log_->warn("failed to retrieve block: {}", *error);
          }
          if (my_height >= target_height) {
            return mutable_factory_->commit(std::move(storage));
          }
          if (not peer_ok) {
            // if the last block did not apply or we got no new blocks from this
            // peer we should switch to next peer
            break;
          }
        }
      }

      return expected::makeError(
          "Failed to download and commit any blocks from given peers");
    }

    iroha::expected::Result<std::unique_ptr<ametsuchi::MutableStorage>,
                            std::string>
    SynchronizerImpl::getStorage() {
      return mutable_factory_->createMutableStorage(command_executor_);
    }

    SynchronizationEvent SynchronizerImpl::processNext(
        const consensus::PairValid &msg) {
      log_->info("at handleNext");
      const auto notify =
          [this,
           &msg](std::shared_ptr<const iroha::LedgerState> &&ledger_state) {
            return SynchronizationEvent{SynchronizationOutcomeType::kCommit,
                                        msg.round,
                                        std::move(ledger_state)};
          };
      if (mutable_factory_->preparedCommitEnabled()) {
        auto result = mutable_factory_->commitPrepared(msg.block);
        if (iroha::expected::hasValue(result)) {
          return notify(std::move(result).assumeValue());
        }
        log_->error("Error committing prepared block: {}",
                    result.assumeError());
      }
      auto commit_result =
          getStorage() | [&](auto &&storage) -> ametsuchi::CommitResult {
        if (storage->apply(msg.block)) {
          return mutable_factory_->commit(std::move(storage));
        } else {
          return "Block failed to apply.";
        }
      };
      return std::move(commit_result)
          .match(
              [&notify](auto &&ledger_state) {
                return notify(std::move(ledger_state.value));
              },
              [this, &msg](const auto &error) {
                this->log_->error("Failed to commit: {}", error.error);
                return SynchronizationEvent{SynchronizationOutcomeType::kError,
                                            msg.round,
                                            msg.ledger_state};
              });
    }

    SynchronizationEvent SynchronizerImpl::processDifferent(
        const consensus::Synchronizable &msg,
        shared_model::interface::types::HeightType required_height) {
      log_->info("at handleDifferent");

      auto commit_result = downloadAndCommitMissingBlocks(
          msg.ledger_state->top_block_info.height,
          required_height,
          msg.public_keys);

      return commit_result.match(
          [this, &msg](auto &value) {
            auto &ledger_state = value.value;
            assert(ledger_state);
            const auto new_height = ledger_state->top_block_info.height;
            return SynchronizationEvent{SynchronizationOutcomeType::kCommit,
                                        new_height != msg.round.block_round
                                            ? consensus::Round{new_height, 0}
                                            : msg.round,
                                        std::move(ledger_state)};
          },
          [this, &msg](const auto &error) {
            log_->error("Synchronization failed: {}", error.error);
            return SynchronizationEvent{SynchronizationOutcomeType::kError,
                                        msg.round,
                                        msg.ledger_state};
          });
    }

  }  // namespace synchronizer
}  // namespace iroha
