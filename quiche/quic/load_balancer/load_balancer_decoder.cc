// Copyright (c) 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/load_balancer/load_balancer_decoder.h"

#include <cstdint>
#include <cstring>
#include <optional>

#include "absl/types/span.h"
#include "quiche/quic/core/quic_connection_id.h"
#include "quiche/quic/load_balancer/load_balancer_config.h"
#include "quiche/quic/load_balancer/load_balancer_server_id.h"
#include "quiche/quic/platform/api/quic_bug_tracker.h"

namespace quic {

bool LoadBalancerDecoder::AddConfig(const LoadBalancerConfig& config) {
  if (config_[config.config_id()].has_value()) {
    return false;
  }
  config_[config.config_id()] = config;
  return true;
}

void LoadBalancerDecoder::DeleteConfig(uint8_t config_id) {
  if (config_id >= kNumLoadBalancerConfigs) {
    QUIC_BUG(quic_bug_438896865_01)
        << "Decoder deleting config with invalid config_id "
        << static_cast<int>(config_id);
    return;
  }
  config_[config_id].reset();
}

// This is the core logic to extract a server ID given a valid config and
// connection ID of sufficient length.
LoadBalancerServerId LoadBalancerDecoder::GetServerId(
    const QuicConnectionId& connection_id) const {
  std::optional<uint8_t> config_id = GetConfigId(connection_id);
  if (!config_id.has_value()) {
    return LoadBalancerServerId();
  }
  std::optional<LoadBalancerConfig> config = config_[*config_id];
  if (!config.has_value()) {
    return LoadBalancerServerId();
  }
  if (connection_id.length() < config->total_len()) {
    // Connection ID wasn't long enough
    return LoadBalancerServerId();
  }
  // The first byte is complete. Finish the rest.
  const uint8_t* data =
      reinterpret_cast<const uint8_t*>(connection_id.data()) + 1;
  if (!config->IsEncrypted()) {  // It's a Plaintext CID.
    return LoadBalancerServerId(
        absl::Span<const uint8_t>(data, config->server_id_len()));
  }
  uint8_t result[kQuicMaxConnectionIdWithLengthPrefixLength];
  if (config->plaintext_len() == kLoadBalancerKeyLen) {  // single pass
    if (!config->BlockDecrypt(data, result)) {
      return LoadBalancerServerId();
    }
  } else {
    // Do 3 or 4 passes. Only 3 are necessary if the server_id is short enough
    // to fit in the first half of the connection ID (the decoder doesn't need
    // to extract the nonce).
    memcpy(result, data, config->plaintext_len());
    uint8_t end = (config->server_id_len() > config->nonce_len()) ? 1 : 2;
    for (uint8_t i = kNumLoadBalancerCryptoPasses; i >= end; i--) {
      if (!config->EncryptionPass(absl::Span<uint8_t>(result), i)) {
        return LoadBalancerServerId();
      }
    }
  }
  return LoadBalancerServerId(
      absl::Span<const uint8_t>(result, config->server_id_len()));
}

std::optional<uint8_t> LoadBalancerDecoder::GetConfigId(
    const QuicConnectionId& connection_id) {
  if (connection_id.IsEmpty()) {
    return std::optional<uint8_t>();
  }
  return GetConfigId(*reinterpret_cast<const uint8_t*>(connection_id.data()));
}

std::optional<uint8_t> LoadBalancerDecoder::GetConfigId(
    const uint8_t connection_id_first_byte) {
  uint8_t codepoint = (connection_id_first_byte >> kConnectionIdLengthBits);
  if (codepoint < kNumLoadBalancerConfigs) {
    return codepoint;
  }
  return std::optional<uint8_t>();
}

}  // namespace quic
