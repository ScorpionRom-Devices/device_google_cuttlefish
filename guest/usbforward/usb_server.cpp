/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// #undef NDEBUG

#include "guest/usbforward/usb_server.h"

#include <string>
#include <vector>
#include <strings.h>
#include <cutils/log.h>
#include <libusb/libusb.h>
#include "common/libs/fs/shared_select.h"
#include "guest/usbforward/protocol.h"
#include "guest/usbforward/transport_request.h"

namespace usb_forward {
namespace {
// USBServer exports device kExportedVendorID:kExportedProductID to the server.
// We will not support exporting multiple USB devices as there's no practical
// need for this.
constexpr uint16_t kExportedVendorID = 0x18d1;
constexpr uint16_t kExportedProductID = 0x4ee7;

// Use default BUS and DEVICE IDs so that it's easier to attach over USB/IP.
constexpr uint8_t kDefaultBusID = 1;
constexpr uint8_t kDefaultDevID = 1;

std::shared_ptr<libusb_device_handle> GetDevice() {
  std::shared_ptr<libusb_device_handle> res(
      libusb_open_device_with_vid_pid(nullptr, kExportedVendorID,
                                      kExportedProductID),
      [](libusb_device_handle* h) {
        // Apparently, deleter is called even on an uninitialized shared_ptr.
        if (h != nullptr) {
          libusb_release_interface(h, 0);
          libusb_close(h);
        }
      });

  if (res) libusb_claim_interface(res.get(), 0);

  return res;
}

bool GetDeviceInfo(DeviceInfo* info, std::vector<InterfaceInfo>* ifaces) {
  auto handle = GetDevice();
  if (!handle) return false;

  // This function does not modify the reference count of the returned device,
  // so do not feel compelled to unreference it when you are done.
  libusb_device* dev = libusb_get_device(handle.get());

  libusb_device_descriptor desc;
  libusb_config_descriptor* conf;
  memset(info, 0, sizeof(*info));

  int res = libusb_get_device_descriptor(dev, &desc);
  if (res < 0) {
    // This shouldn't really happen.
    ALOGE("libusb_get_device_descriptor failed %d", res);
    return false;
  }

  res = libusb_get_active_config_descriptor(dev, &conf);
  if (res < 0) {
    // This shouldn't really happen.
    ALOGE("libusb_get_active_config_descriptor failed %d", res);
    libusb_free_config_descriptor(conf);
    return false;
  }

  info->vendor_id = desc.idVendor;
  info->product_id = desc.idProduct;
  info->dev_version = desc.bcdDevice;
  info->dev_class = desc.bDeviceClass;
  info->dev_subclass = desc.bDeviceSubClass;
  info->dev_protocol = desc.bDeviceProtocol;
  info->speed = libusb_get_device_speed(dev);
  info->num_configurations = desc.bNumConfigurations;
  info->num_interfaces = conf->bNumInterfaces;
  info->cur_configuration = conf->bConfigurationValue;
  info->bus_id = kDefaultBusID;
  info->dev_id = kDefaultDevID;

  if (ifaces != nullptr) {
    for (int ifidx = 0; ifidx < conf->bNumInterfaces; ++ifidx) {
      const libusb_interface& iface = conf->interface[ifidx];
      for (int altidx = 0; altidx < iface.num_altsetting; ++altidx) {
        const libusb_interface_descriptor& alt = iface.altsetting[altidx];
        ifaces->push_back(InterfaceInfo{alt.bInterfaceClass,
                                        alt.bInterfaceSubClass,
                                        alt.bInterfaceProtocol, 0});
      }
    }
  }
  libusb_free_config_descriptor(conf);
  return true;
}

void* ProcessLibUSBRequests(void* discard) {
  while (true) {
    libusb_handle_events_completed(nullptr, nullptr);
  }
}
}  // anonymous namespace

USBServer::USBServer(const avd::SharedFD& fd)
    : handle_{nullptr, libusb_close},
      libusb_thread_(&ProcessLibUSBRequests, nullptr),
      fd_{fd} {}

void USBServer::HandleDeviceList(uint32_t tag) {
  // Iterate all devices and send structure for every found device.
  // Write header: number of devices.
  DeviceInfo info;
  std::vector<InterfaceInfo> ifaces;
  bool found = GetDeviceInfo(&info, &ifaces);

  avd::LockGuard<avd::Mutex> lock(write_mutex_);
  ResponseHeader rsp{StatusSuccess, tag};
  fd_->Write(&rsp, sizeof(rsp));
  if (found) {
    uint32_t cnt = 1;
    fd_->Write(&cnt, sizeof(cnt));
    fd_->Write(&info, sizeof(info));
    fd_->Write(ifaces.data(), ifaces.size() * sizeof(InterfaceInfo));
  } else {
    // No devices.
    uint32_t cnt = 0;
    fd_->Write(&cnt, sizeof(cnt));
  }
}

void USBServer::HandleAttach(uint32_t tag) {
  handle_ = GetDevice();

  // We read the request, but it no longer plays any significant role here.
  AttachRequest req;
  if (fd_->Read(&req, sizeof(req)) != sizeof(req)) return;

  avd::LockGuard<avd::Mutex> lock(write_mutex_);
  ResponseHeader rsp{handle_ ? StatusSuccess : StatusFailure, tag};
  fd_->Write(&rsp, sizeof(rsp));
}

void USBServer::HandleHeartbeat(uint32_t tag) {
  auto handle = GetDevice();

  avd::LockGuard<avd::Mutex> lock(write_mutex_);
  ResponseHeader rsp{handle ? StatusSuccess : StatusFailure, tag};
  fd_->Write(&rsp, sizeof(rsp));
}

void USBServer::HandleControlTransfer(uint32_t tag) {
  ControlTransfer req;
  // If disconnected prematurely, don't send response.
  if (fd_->Read(&req, sizeof(req)) != sizeof(req)) return;

  // Technically speaking this isn't endpoint, but names, masks, values and
  // meaning here is exactly same.
  bool is_data_in =
      ((req.type & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN);

  std::unique_ptr<TransportRequest> treq(new TransportRequest(
      handle_,
      [this, is_data_in, tag](bool is_success, const uint8_t* data,
                              int32_t length) {
        OnTransferComplete(tag, is_data_in, is_success, data, length);
      },
      req));

  if (!is_data_in && req.length) {
    // If disconnected prematurely, don't send response.
    int32_t got = 0;
    while (got < req.length) {
      auto read = fd_->Read(&treq->Buffer()[got], req.length - got);
      if (fd_->GetErrno() != 0) {
        ALOGE("Failed to read from client: %s", fd_->StrError());
        return;
      } else if (read == 0) {
        ALOGE("Failed to read from client: short read");
        return;
      }
      got += read;
    }
  }

  // At this point we store transport request internally until it completes.
  TransportRequest* treq_ptr = treq.get();
  {
    avd::LockGuard<avd::Mutex> lock(requests_mutex_);
    requests_in_flight_[tag] = std::move(treq);
  }

  if (!treq_ptr->Submit()) {
    OnTransferComplete(tag, is_data_in, false, nullptr, 0);
  }
}

void USBServer::HandleDataTransfer(uint32_t tag) {
  DataTransfer req;
  // If disconnected prematurely, don't send response.
  if (fd_->Read(&req, sizeof(req)) != sizeof(req)) return;

  bool is_data_in = !req.is_host_to_device;

  std::unique_ptr<TransportRequest> treq(new TransportRequest(
      handle_,
      [this, is_data_in, tag](bool is_success, const uint8_t* data,
                              int32_t length) {
        OnTransferComplete(tag, is_data_in, is_success, data, length);
      },
      req));

  if (!is_data_in && req.length) {
    // If disconnected prematurely, don't send response.
    int32_t got = 0;
    while (got < req.length) {
      auto read = fd_->Read(&treq->Buffer()[got], req.length - got);
      if (fd_->GetErrno() != 0) {
        ALOGE("Failed to read from client: %s", fd_->StrError());
        return;
      } else if (read == 0) {
        ALOGE("Failed to read from client: short read");
        return;
      }
      got += read;
    }
  }

  // At this point we store transport request internally until it completes.
  TransportRequest* treq_ptr = treq.get();
  {
    avd::LockGuard<avd::Mutex> lock(requests_mutex_);
    requests_in_flight_[tag] = std::move(treq);
  }

  if (!treq_ptr->Submit()) {
    OnTransferComplete(tag, is_data_in, false, nullptr, 0);
  }
}

void USBServer::OnTransferComplete(uint32_t tag, bool is_data_in,
                                   bool is_success, const uint8_t* buffer,
                                   int32_t actual_length) {
  ResponseHeader rsp{is_success ? StatusSuccess : StatusFailure, tag};

  avd::LockGuard<avd::Mutex> lock(write_mutex_);
  fd_->Write(&rsp, sizeof(rsp));
  if (is_success && is_data_in) {
    fd_->Write(&actual_length, sizeof(actual_length));
    if (actual_length > 0) {
      // NOTE: don't use buffer_ here directly, as libusb uses first few bytes
      // to store control data there.
      int32_t sent = 0;
      while (sent < actual_length) {
        int packet_size = fd_->Write(&buffer[sent], actual_length - sent);
        sent += packet_size;
        ALOGV("Sending response, %d / %d bytes sent", sent, actual_length);
        if (fd_->GetErrno() != 0) {
          ALOGE("Send failed: %s", fd_->StrError());
          return;
        }
      }
    }
  }

  {
    avd::LockGuard<avd::Mutex> lock(requests_mutex_);
    requests_in_flight_.erase(tag);
  }
}

int USBServer::HandleDeviceEvent(libusb_context*, libusb_device*,
                                 libusb_hotplug_event event, void* self_raw) {
  auto self = reinterpret_cast<USBServer*>(self_raw);
  switch (event) {
    case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED:
      self->handle_ = GetDevice();
      break;
    case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT:
      self->handle_.reset();
      break;
  }
  return 0;
}

void USBServer::Serve() {
  // Need to check if device is still there before we execute anything.
  libusb_hotplug_callback_handle handle;
  libusb_hotplug_register_callback(
      nullptr,
      libusb_hotplug_event(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                           LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
      libusb_hotplug_flag(0), kExportedVendorID, kExportedProductID,
      LIBUSB_HOTPLUG_MATCH_ANY, &USBServer::HandleDeviceEvent, this, &handle);

  avd::SharedFDSet rset;
  while (true) {
    rset.Zero();
    rset.Set(fd_);
    int ret = avd::Select(&rset, nullptr, nullptr, nullptr);

    if (ret < 0) continue;

    if (rset.IsSet(fd_)) {
      RequestHeader req;
      if (fd_->Read(&req, sizeof(req)) != sizeof(req)) {
        // There's nobody on the other side.
        sleep(3);
        continue;
      }

      switch (req.command) {
        case CmdDeviceList:
          ALOGV("Processing DeviceList command, tag=%d", req.tag);
          HandleDeviceList(req.tag);
          break;

        case CmdAttach:
          ALOGV("Processing Attach command, tag=%d", req.tag);
          HandleAttach(req.tag);
          break;

        case CmdControlTransfer:
          ALOGV("Processing ControlTransfer command, tag=%d", req.tag);
          HandleControlTransfer(req.tag);
          break;

        case CmdDataTransfer:
          ALOGV("Processing DataTransfer command, tag=%d", req.tag);
          HandleDataTransfer(req.tag);
          break;

        case CmdHeartbeat:
          ALOGV("Processing Heartbeat command, tag=%d", req.tag);
          HandleHeartbeat(req.tag);
          break;

        default:
          ALOGE("Discarding unknown command %08x, tag=%d", req.command,
                req.tag);
      }
    }
  }
}

}  // namespace usb_forward
