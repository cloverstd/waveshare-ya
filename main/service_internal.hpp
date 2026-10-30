#pragma once

namespace ya {
// Owned by the service task; these functions never run under the GUI lock.
void board_service_tick();
void time_service_init();
void time_service_tick();
void time_service_request_sync();
} // namespace ya
