#include "core_v2/internal/p2p_cluster_pcp_handler.h"

#include "core_v2/internal/bluetooth_endpoint_channel.h"
#include "core_v2/internal/wifi_lan_endpoint_channel.h"
#include "platform_v2/public/crypto.h"
#include "proto/connections_enums.pb.h"

namespace location {
namespace nearby {
namespace connections {

ByteArray P2pClusterPcpHandler::GenerateHash(const std::string& source,
                                             size_t size) {
  ByteArray full_hash = Crypto::Sha256(source);
  ByteArray result(size);
  result.CopyAt(0, full_hash);
  return result;
}

P2pClusterPcpHandler::P2pClusterPcpHandler(
    Mediums& mediums, EndpointManager* endpoint_manager,
    EndpointChannelManager* endpoint_channel_manager, Pcp pcp)
    : BasePcpHandler(endpoint_manager, endpoint_channel_manager, pcp),
      bluetooth_radio_(mediums.GetBluetoothRadio()),
      bluetooth_medium_(mediums.GetBluetoothClassic()),
      wifi_lan_medium_(mediums.GetWifiLan()) {}

// Returns a vector or mediums sorted in order or decreasing priority for
// all the supported mediums.
// NOTE: currently we only have BT, but eventually it will be more, and items
// will have to be sorted in the order of decreasing traffic bandwidth.
// Example: WiFi_LAN, BT, BLE
std::vector<proto::connections::Medium>
P2pClusterPcpHandler::GetConnectionMediumsByPriority() {
  std::vector<proto::connections::Medium> mediums;
  if (bluetooth_medium_.IsAvailable()) {
    mediums.push_back(proto::connections::BLUETOOTH);
  }
  if (wifi_lan_medium_.IsAvailable()) {
    mediums.push_back(proto::connections::WIFI_LAN);
  }
  return mediums;
}

proto::connections::Medium P2pClusterPcpHandler::GetDefaultUpgradeMedium() {
  return proto::connections::WIFI_LAN;
}

BasePcpHandler::StartOperationResult P2pClusterPcpHandler::StartAdvertisingImpl(
    ClientProxy* client, const std::string& service_id,
    const std::string& local_endpoint_id,
    const std::string& local_endpoint_name, const ConnectionOptions& options) {
  std::vector<proto::connections::Medium> mediums_started_successfully;

  const ByteArray bluetooth_hash =
      GenerateHash(service_id, BluetoothDeviceName::kServiceIdHashLength);
  proto::connections::Medium bluetooth_medium =
      StartBluetoothAdvertising(client, service_id, bluetooth_hash,
                                local_endpoint_id, local_endpoint_name);
  if (bluetooth_medium != proto::connections::UNKNOWN_MEDIUM) {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartAdvertisingImpl: BT added");
    mediums_started_successfully.push_back(bluetooth_medium);
  }

  const ByteArray wifi_lan_hash =
      GenerateHash(service_id, WifiLanServiceInfo::kServiceIdHashLength);
  proto::connections::Medium wifi_lan_medium =
      StartWifiLanAdvertising(client, service_id, wifi_lan_hash,
                              local_endpoint_id, local_endpoint_name);
  if (wifi_lan_medium != proto::connections::UNKNOWN_MEDIUM) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartAdvertisingImpl: WifiLan added");
    mediums_started_successfully.push_back(wifi_lan_medium);
  }

  if (mediums_started_successfully.empty()) {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartAdvertisingImpl: not started");
    return {
        .status = {Status::kBluetoothError},
    };
  }

  // The rest of the operations for startAdvertising() will continue
  // asynchronously via
  // IncomingBluetoothConnectionProcessor.onIncomingBluetoothConnection(), so
  // leave it to that to signal any errors that may occur.
  return {
      .status = {Status::kSuccess},
      .mediums = std::move(mediums_started_successfully),
  };
}

Status P2pClusterPcpHandler::StopAdvertisingImpl(ClientProxy* client) {
  wifi_lan_medium_.StopAdvertising(client->GetAdvertisingServiceId());
  bluetooth_medium_.TurnOffDiscoverability();
  bluetooth_medium_.StopAcceptingConnections(client->GetAdvertisingServiceId());
  return {Status::kSuccess};
}

bool P2pClusterPcpHandler::IsRecognizedBluetoothEndpoint(
    const std::string& name_string, const std::string& service_id,
    const BluetoothDeviceName& name) const {
  if (!name.IsValid()) {
    NEARBY_LOG(
        INFO,
        "P2pClusterPcpHandler::IsRecognizedBluetoothEndpoint: name is invalid");
    return false;
  }

  if (name.GetPcp() != GetPcp()) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::IsRecognizedBluetoothEndpoint: Pcp is "
               "not matched; name.Pcp=%d, Pcp=%d",
               name.GetPcp(), GetPcp());
    return false;
  }

  ByteArray expected_service_id_hash =
      GenerateHash(service_id, BluetoothDeviceName::kServiceIdHashLength);

  if (name.GetServiceIdHash() != expected_service_id_hash) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::IsRecognizedBluetoothEndpoint: service "
               "id hash is "
               "not matched; name.service_id_hash=%s, expected=%s",
               name.GetServiceIdHash().data(), expected_service_id_hash.data());
    return false;
  }

  return true;
}

std::function<void(BluetoothDevice&)>
P2pClusterPcpHandler::MakeBluetoothDeviceDiscoveredHandler(
    ClientProxy* client, const std::string& service_id) {
  return [this, client, service_id](BluetoothDevice& device) {
    RunOnPcpHandlerThread([this, client, service_id, &device]() {
      // Make sure we are still discovering before proceeding.
      if (!client->IsDiscovering()) {
        NEARBY_LOG(INFO,
                   "BT discovery handler (FOUND) [client=%p, service=%s]: not "
                   "in discovery mode",
                   client, service_id.c_str());
        return;
      }

      // Parse the Bluetooth device name.
      const std::string& device_name_string = device.GetName();
      BluetoothDeviceName device_name(device_name_string);

      // Make sure the Bluetooth device name points to a valid
      // endpoint we're discovering.
      if (!IsRecognizedBluetoothEndpoint(device_name_string, service_id,
                                         device_name))
        return;

      // Report the discovered endpoint to the client.
      NEARBY_LOG(INFO,
                 "Invoking BasePcpHandler::OnEndpointFound() for BT "
                 "service=%s; id=%s; name=%s",
                 service_id.c_str(), device_name.GetEndpointId().c_str(),
                 device_name.GetEndpointName().c_str());
      OnEndpointFound(client,
                      std::make_shared<BluetoothEndpoint>(BluetoothEndpoint{
                          {
                              .endpoint_id = device_name.GetEndpointId(),
                              .endpoint_name = device_name.GetEndpointName(),
                              .service_id = service_id,
                              .medium = proto::connections::Medium::BLUETOOTH,
                          },
                          device,
                      }));
    });
  };
}

std::function<void(BluetoothDevice&)>
P2pClusterPcpHandler::MakeBluetoothDeviceLostHandler(
    ClientProxy* client, const std::string& service_id) {
  return [this, client, service_id](BluetoothDevice& device) {
    RunOnPcpHandlerThread([this, client, &service_id, &device]() {
      // Make sure we are still discovering before proceeding.
      if (!client->IsDiscovering()) {
        NEARBY_LOG(INFO,
                   "BT discovery handler (LOST) [client=%p, service=%s]: not "
                   "in discovery mode",
                   client, service_id.c_str());
        return;
      }

      // Parse the Bluetooth device name.
      const std::string& device_name_string = device.GetName();
      BluetoothDeviceName device_name(device_name_string);

      // Make sure the Bluetooth device name points to a valid
      // endpoint we're discovering.
      if (!IsRecognizedBluetoothEndpoint(device_name_string, service_id,
                                         device_name))
        return;

      // Report the discovered endpoint to the client.
      NEARBY_LOG(INFO,
                 "BT discovery handler (LOST) [client=%p, service=%s]: report "
                 "to client",
                 client, service_id.c_str());
      OnEndpointLost(client,
                     BluetoothEndpoint{
                         {
                             .endpoint_id = device_name.GetEndpointId(),
                             .endpoint_name = device_name.GetEndpointName(),
                             .service_id = service_id,
                             .medium = proto::connections::Medium::BLUETOOTH,
                         },
                         device,
                     });
    });
  };
}

bool P2pClusterPcpHandler::IsRecognizedWifiLanEndpoint(
    const std::string& name_string, const std::string& service_id,
    const WifiLanServiceInfo& name) const {
  if (!name.IsValid()) {
    NEARBY_LOG(
        INFO,
        "P2pClusterPcpHandler::IsRecognizedWifiLanEndpoint: name is invalid");
    return false;
  }

  if (name.GetPcp() != GetPcp()) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::IsRecognizedWifiLanEndpoint: Pcp is "
               "not matched; name.Pcp=%d, Pcp=%d",
               name.GetPcp(), GetPcp());
    return false;
  }

  ByteArray expected_service_id_hash =
      GenerateHash(service_id, BluetoothDeviceName::kServiceIdHashLength);

  if (name.GetServiceIdHash() != expected_service_id_hash) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::IsRecognizedWifiLanEndpoint: service "
               "id hash is "
               "not matched; name.service_id_hash=%s, expected=%s",
               name.GetServiceIdHash().data(), expected_service_id_hash.data());
    return false;
  }

  return true;
}

std::function<void(WifiLanService&, const std::string&)>
P2pClusterPcpHandler::MakeWifiLanServiceDiscoveredHandler(
    ClientProxy* client, const std::string& service_id) {
  return [this, client](WifiLanService& service,
                        const std::string& service_id) {
    RunOnPcpHandlerThread([this, client, service_id, &service]() {
      // Make sure we are still discovering before proceeding.
      if (!client->IsDiscovering()) {
        NEARBY_LOG(
            INFO,
            "WifiLan discovery handler (FOUND) [client=%p, service=%s]: not "
            "in discovery mode",
            client, service_id.c_str());
        return;
      }

      // Parse the WifiLan service name.
      const std::string& service_name_string = service.GetName();
      WifiLanServiceInfo service_name(service_name_string);

      // Make sure the WifiLan service name points to a valid
      // endpoint we're discovering.
      if (!IsRecognizedWifiLanEndpoint(service_name_string, service_id,
                                       service_name))
        return;

      // Report the discovered endpoint to the client.
      NEARBY_LOG(INFO,
                 "Invoking BasePcpHandler::OnEndpointFound() for WifiLan "
                 "service=%s; id=%s; name=%s",
                 service_id.c_str(), service_name.GetEndpointId().c_str(),
                 service_name.GetEndpointName().c_str());
      OnEndpointFound(client,
                      std::make_shared<WifiLanEndpoint>(WifiLanEndpoint{
                          {
                              .endpoint_id = service_name.GetEndpointId(),
                              .endpoint_name = service_name.GetEndpointName(),
                              .service_id = service_id,
                              .medium = proto::connections::Medium::WIFI_LAN,
                          },
                          service,
                      }));
    });
  };
}

std::function<void(WifiLanService&, const std::string&)>
P2pClusterPcpHandler::MakeWifiLanServiceLostHandler(
    ClientProxy* client, const std::string& service_id) {
  return [this, client](WifiLanService& service,
                        const std::string& service_id) {
    RunOnPcpHandlerThread([this, client, &service_id, &service]() {
      // Make sure we are still discovering before proceeding.
      if (!client->IsDiscovering()) {
        NEARBY_LOG(
            INFO,
            "WifiLan discovery handler (LOST) [client=%p, service=%s]: not "
            "in discovery mode",
            client, service_id.c_str());
        return;
      }

      // Parse the WifiLan service name.
      const std::string& service_name_string = service.GetName();
      WifiLanServiceInfo service_name(service_name_string);

      // Make sure the WifiLan service name points to a valid
      // endpoint we're discovering.
      if (!IsRecognizedWifiLanEndpoint(service_name_string, service_id,
                                       service_name))
        return;

      // Report the discovered endpoint to the client.
      NEARBY_LOG(
          INFO,
          "WifiLan discovery handler (LOST) [client=%p, service=%s]: report "
          "to client",
          client, service_id.c_str());
      OnEndpointLost(client,
                     WifiLanEndpoint{
                         {
                             .endpoint_id = service_name.GetEndpointId(),
                             .endpoint_name = service_name.GetEndpointName(),
                             .service_id = service_id,
                             .medium = proto::connections::Medium::WIFI_LAN,
                         },
                         service,
                     });
    });
  };
}

BasePcpHandler::StartOperationResult P2pClusterPcpHandler::StartDiscoveryImpl(
    ClientProxy* client, const std::string& service_id,
    const ConnectionOptions& options) {
  std::vector<proto::connections::Medium> mediums_started_successfully;

  proto::connections::Medium bluetooth_medium = StartBluetoothDiscovery(
      {
          .device_discovered_cb =
              MakeBluetoothDeviceDiscoveredHandler(client, service_id),
          .device_lost_cb = MakeBluetoothDeviceLostHandler(client, service_id),
      },
      client, service_id);
  if (bluetooth_medium != proto::connections::UNKNOWN_MEDIUM) {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartDiscoveryImpl: BT added");
    mediums_started_successfully.push_back(bluetooth_medium);
  }

  proto::connections::Medium wifi_lan_medium = StartWifiLanDiscovery(
      {
          .service_discovered_cb =
              MakeWifiLanServiceDiscoveredHandler(client, service_id),
          .service_lost_cb = MakeWifiLanServiceLostHandler(client, service_id),
      },
      client, service_id);
  if (wifi_lan_medium != proto::connections::UNKNOWN_MEDIUM) {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartDiscoveryImpl: WifiLan added");
    mediums_started_successfully.push_back(wifi_lan_medium);
  }

  if (mediums_started_successfully.empty()) {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartDiscoveryImpl: nothing added");
    return {
        .status = {Status::kBluetoothError},
    };
  }

  return {
      .status = {Status::kSuccess},
      .mediums = std::move(mediums_started_successfully),
  };
}

Status P2pClusterPcpHandler::StopDiscoveryImpl(ClientProxy* client) {
  wifi_lan_medium_.StopDiscovery(client->GetDiscoveryServiceId());
  bluetooth_medium_.StopDiscovery();
  return {Status::kSuccess};
}

BasePcpHandler::ConnectImplResult P2pClusterPcpHandler::ConnectImpl(
    ClientProxy* client, BasePcpHandler::DiscoveredEndpoint* endpoint) {
  BluetoothEndpoint* bluetooth_endpoint =
      static_cast<BluetoothEndpoint*>(endpoint);
  if (bluetooth_endpoint) {
    return BluetoothConnectImpl(client, bluetooth_endpoint);
  }

  WifiLanEndpoint* wifi_lan_endpoint = static_cast<WifiLanEndpoint*>(endpoint);
  if (wifi_lan_endpoint) {
    return WifiLanConnectImpl(client, wifi_lan_endpoint);
  }

  return BasePcpHandler::ConnectImplResult{
      .status = {Status::kError},
  };
}

proto::connections::Medium P2pClusterPcpHandler::StartBluetoothAdvertising(
    ClientProxy* client, const std::string& service_id,
    const ByteArray& service_id_hash, const std::string& local_endpoint_id,
    const std::string& local_endpoint_name) {
  // Start listening for connections before advertising in case a connection
  // request comes in very quickly.
  NEARBY_LOG(
      INFO,
      "P2pClusterPcpHandler::StartBluetoothAdvertising: service=%s: start",
      service_id.c_str());
  if (bluetooth_medium_.IsAcceptingConnections(service_id)) {
    NEARBY_LOG(ERROR, "BT is already accepting connections for service=%s",
               service_id.c_str());
    return proto::connections::UNKNOWN_MEDIUM;
  }

  NEARBY_LOG(
      INFO,
      "P2pClusterPcpHandler::StartBluetoothAdvertising: service=%s: invoking",
      service_id.c_str());
  if (!bluetooth_radio_.Enable() ||
      !bluetooth_medium_.StartAcceptingConnections(
          service_id, {.accepted_cb = [this, client, local_endpoint_name](
                                          BluetoothSocket socket) {
            if (!socket.IsValid()) {
              NEARBY_LOG(ERROR, "Invalid socket in accept callback: name=%s",
                         local_endpoint_name.c_str());
              return;
            }
            RunOnPcpHandlerThread([this, client, local_endpoint_name,
                                   socket = std::move(socket)]() mutable {
              std::string remote_device_name =
                  socket.GetRemoteDevice().GetName();
              auto channel = absl::make_unique<BluetoothEndpointChannel>(
                  remote_device_name, socket);
              OnIncomingConnection(client, remote_device_name,
                                   std::move(channel),
                                   proto::connections::Medium::BLUETOOTH);
            });
          }})) {
    NEARBY_LOG(ERROR, "BT failed to start accepting connections for service=%s",
               service_id.c_str());
    return proto::connections::UNKNOWN_MEDIUM;
  }

  NEARBY_LOG(INFO,
             "P2pClusterPcpHandler::StartBluetoothAdvertising: service=%s: "
             "make name; id=%s, hash=%s, name=%s",
             service_id.c_str(), local_endpoint_id.c_str(),
             std::string(service_id_hash).c_str(), local_endpoint_name.c_str());
  // Generate a BluetoothDeviceName with which to become Bluetooth discoverable.
  std::string device_name(BluetoothDeviceName(
      BluetoothDeviceName::Version::kV1, GetPcp(), local_endpoint_id,
      service_id_hash, local_endpoint_name));
  if (device_name.empty()) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartBluetoothAdvertising: generate "
               "BluetoothDeviceName failed");
    bluetooth_medium_.StopAcceptingConnections(service_id);
    return proto::connections::UNKNOWN_MEDIUM;
  } else {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartBluetoothAdvertising: generate "
               "BluetoothDeviceName succeeded; device_name=%s",
               device_name.c_str());
  }

  NEARBY_LOG(
      INFO,
      "P2pClusterPcpHandler::StartBluetoothAdvertising: service=%s: come up",
      service_id.c_str());
  // Become Bluetooth discoverable.
  if (!bluetooth_medium_.TurnOnDiscoverability(device_name)) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartBluetoothAdvertising: failed to "
               "turn on discoverability, device_name=%s",
               device_name.c_str());
    bluetooth_medium_.StopAcceptingConnections(service_id);
    return proto::connections::UNKNOWN_MEDIUM;
  } else {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartBluetoothAdvertising: succeeded to "
               "turn on discoverability, device_name=%s",
               device_name.c_str());
  }
  NEARBY_LOG(
      INFO, "P2pClusterPcpHandler::StartBluetoothAdvertising: service=%s: done",
      service_id.c_str());
  return proto::connections::BLUETOOTH;
}

proto::connections::Medium P2pClusterPcpHandler::StartBluetoothDiscovery(
    BluetoothDiscoveredDeviceCallback callback, ClientProxy* client,
    const std::string& service_id) {
  if (bluetooth_radio_.Enable() &&
      bluetooth_medium_.StartDiscovery(std::move(callback))) {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartBluetoothDiscovery: ok");
    return proto::connections::BLUETOOTH;
  } else {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartBluetoothDiscovery: failed");
    return proto::connections::UNKNOWN_MEDIUM;
  }
}

BasePcpHandler::ConnectImplResult P2pClusterPcpHandler::BluetoothConnectImpl(
    ClientProxy* client, BluetoothEndpoint* endpoint) {
  BluetoothDevice& device = endpoint->bluetooth_device;

  BluetoothSocket bluetooth_socket =
      bluetooth_medium_.Connect(device, endpoint->service_id);
  if (!bluetooth_socket.IsValid()) {
    return BasePcpHandler::ConnectImplResult{
        .status = {Status::kBluetoothError},
    };
  }

  auto channel = absl::make_unique<BluetoothEndpointChannel>(
      endpoint->endpoint_id, bluetooth_socket);

  return BasePcpHandler::ConnectImplResult{
      .medium = proto::connections::Medium::BLUETOOTH,
      .status = {Status::kSuccess},
      .endpoint_channel = std::move(channel),
  };
}

proto::connections::Medium P2pClusterPcpHandler::StartWifiLanAdvertising(
    ClientProxy* client, const std::string& service_id,
    const ByteArray& service_id_hash, const std::string& local_endpoint_id,
    const std::string& local_endpoint_name) {
  // Start listening for connections before advertising in case a connection
  // request comes in very quickly.
  NEARBY_LOG(INFO,
             "P2pClusterPcpHandler::StartWifiLanAdvertising: service=%s: start",
             service_id.c_str());
  if (wifi_lan_medium_.IsAcceptingConnections(service_id)) {
    NEARBY_LOG(ERROR, "WifiLan is already accepting connections for service=%s",
               service_id.c_str());
    return proto::connections::UNKNOWN_MEDIUM;
  }

  NEARBY_LOG(
      INFO,
      "P2pClusterPcpHandler::StartWifiLanAdvertising: service=%s: invoking",
      service_id.c_str());
  if (!wifi_lan_medium_.StartAcceptingConnections(
          service_id, {.accepted_cb = [this, client, local_endpoint_name](
                                          WifiLanSocket& socket,
                                          const std::string& service_id) {
            if (!socket.IsValid()) {
              NEARBY_LOG(ERROR, "Invalid socket in accept callback: name=%s",
                         local_endpoint_name.c_str());
              return;
            }
            RunOnPcpHandlerThread([this, client, local_endpoint_name,
                                   socket = std::move(socket)]() mutable {
              std::string remote_service_name =
                  socket.GetRemoteWifiLanService().GetName();
              auto channel = absl::make_unique<WifiLanEndpointChannel>(
                  remote_service_name, socket);
              OnIncomingConnection(client, remote_service_name,
                                   std::move(channel),
                                   proto::connections::Medium::WIFI_LAN);
            });
          }})) {
    NEARBY_LOG(ERROR,
               "WifiLan failed to start accepting connections for service=%s",
               service_id.c_str());
    return proto::connections::UNKNOWN_MEDIUM;
  }

  NEARBY_LOG(INFO,
             "P2pClusterPcpHandler::StartWifiLanAdvertising: service=%s: "
             "make name; id=%s, hash=%s, name=%s",
             service_id.c_str(), local_endpoint_id.c_str(),
             std::string(service_id_hash).c_str(), local_endpoint_name.c_str());
  // Generate a WifiLanServiceInfo with which to become WifiLan discoverable.
  std::string service_name(WifiLanServiceInfo(
      WifiLanServiceInfo::Version::kV1, GetPcp(), local_endpoint_id,
      service_id_hash, local_endpoint_name));
  if (service_name.empty()) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartWifiLanAdvertising: generate "
               "WifiLanServiceInfo failed");
    wifi_lan_medium_.StopAcceptingConnections(service_id);
    return proto::connections::UNKNOWN_MEDIUM;
  } else {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartWifiLanAdvertising: generate "
               "WifiLanServiceInfo succeeded; service_name=%s",
               service_name.c_str());
  }

  NEARBY_LOG(
      INFO,
      "P2pClusterPcpHandler::StartWifiLanAdvertising: service=%s: come up",
      service_id.c_str());

  if (!wifi_lan_medium_.StartAdvertising(service_id, service_name)) {
    NEARBY_LOG(INFO,
               "P2pClusterPcpHandler::StartWifiLanAdvertising: failed to "
               "start advertising, service_name=%s",
               service_name.c_str());
    wifi_lan_medium_.StopAcceptingConnections(service_id);
    return proto::connections::UNKNOWN_MEDIUM;
  }
  NEARBY_LOG(INFO,
             "P2pClusterPcpHandler::StartWifiLanAdvertising: service=%s: done",
             service_id.c_str());
  return proto::connections::WIFI_LAN;
}

proto::connections::Medium P2pClusterPcpHandler::StartWifiLanDiscovery(
    WifiLanDiscoveredServiceCallback callback, ClientProxy* client,
    const std::string& service_id) {
  if (wifi_lan_medium_.StartDiscovery(service_id, std::move(callback))) {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartWifiLanDiscovery: ok");
    return proto::connections::WIFI_LAN;
  } else {
    NEARBY_LOG(INFO, "P2pClusterPcpHandler::StartWifiLanDiscovery: failed");
    return proto::connections::UNKNOWN_MEDIUM;
  }
}

BasePcpHandler::ConnectImplResult P2pClusterPcpHandler::WifiLanConnectImpl(
    ClientProxy* client, WifiLanEndpoint* endpoint) {
  WifiLanService& service = endpoint->wifi_lan_service;

  WifiLanSocket wifi_lan_socket =
      wifi_lan_medium_.Connect(service, endpoint->service_id);
  if (!wifi_lan_socket.IsValid()) {
    return BasePcpHandler::ConnectImplResult{
        .status = {Status::kWifiLanError},
    };
  }

  auto channel = absl::make_unique<WifiLanEndpointChannel>(
      endpoint->endpoint_id, wifi_lan_socket);

  return BasePcpHandler::ConnectImplResult{
      .medium = proto::connections::Medium::WIFI_LAN,
      .status = {Status::kSuccess},
      .endpoint_channel = std::move(channel),
  };
}

}  // namespace connections
}  // namespace nearby
}  // namespace location
