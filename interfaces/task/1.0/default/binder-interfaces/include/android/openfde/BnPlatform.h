#ifndef AIDL_GENERATED_LINEAGEOS_WAYDROID_BN_PLATFORM_H_
#define AIDL_GENERATED_LINEAGEOS_WAYDROID_BN_PLATFORM_H_

#include <binder/IInterface.h>
#include <android/openfde/IPlatform.h>

namespace android {

namespace openfde {

class BnPlatform : public ::android::BnInterface<IPlatform> {
public:
  ::android::status_t onTransact(uint32_t _aidl_code, const ::android::Parcel& _aidl_data, ::android::Parcel* _aidl_reply, uint32_t _aidl_flags) override;
};  // class BnPlatform

}  // namespace waydroid

}  // namespace lineageos

#endif  // AIDL_GENERATED_LINEAGEOS_WAYDROID_BN_PLATFORM_H_
