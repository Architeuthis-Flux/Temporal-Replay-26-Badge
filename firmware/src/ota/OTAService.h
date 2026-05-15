// OTAService.h — Scheduler service for BadgeOTA + AssetRegistry.
//
// On each WiFi-up edge, defers an OTA check and a community-registry
// refresh with a short gap between them so two TLS handshakes do not
// overlap (avoids mbedTLS / internal-heap OOM on association). Also
// latches the fresh-install rollback marker once the GUI has been
// alive for 30 s.

#pragma once

#include "../infra/Scheduler.h"

namespace ota {

class OTAService : public IService {
 public:
  void service() override;
  const char* name() const override { return "OTA"; }
};

extern OTAService otaService;

}  // namespace ota
