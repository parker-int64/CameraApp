#pragma once

#include <memory>
#include <string>

#include "base_viewmodel.h"
#include "services/app_services.h"

namespace viewmodel {

class CameraViewModel : public BaseViewModel {
 public:
  explicit CameraViewModel(std::shared_ptr<service::AppServices> services = nullptr);

  std::string get_title() const override { return "Camera"; }
  void on_enter() override;
  void on_exit() override;
  void update(uint32_t delta_ms) override;
  bool handle_action(app::AppAction action) override;

  bool consume_frame(service::CameraFramePtr& frame);
  bool consume_capture_feedback();
  service::CaptureState consume_capture_state(std::string* path = nullptr);
  service::CameraZoomState zoom_state() const;
  std::string status_text() const;

 private:
  std::shared_ptr<service::AppServices> services_;
  service::CameraFramePtr latest_frame_;
  bool capture_feedback_{false};
  service::CaptureState capture_state_{service::CaptureState::Idle};
  std::string capture_path_;
};

}  // namespace viewmodel
