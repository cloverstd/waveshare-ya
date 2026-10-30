#!/usr/bin/env python3
"""Mac BLE peripheral fixture: Environmental Sensing, temperature 22.50 C."""

import CoreBluetooth as CB
from Foundation import NSObject, NSData, NSRunLoop, NSDate
import objc, time


class Peripheral(
    NSObject, protocols=[objc.protocolNamed("CBPeripheralManagerDelegate")]
):
    def peripheralManagerDidUpdateState_(self, manager):
        if manager.state() != CB.CBManagerStatePoweredOn:
            return
        self.characteristic = CB.CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
            CB.CBUUID.UUIDWithString_("2A6E"),
            CB.CBCharacteristicPropertyRead | CB.CBCharacteristicPropertyNotify,
            None,
            CB.CBAttributePermissionsReadable,
        )
        self.service = CB.CBMutableService.alloc().initWithType_primary_(
            CB.CBUUID.UUIDWithString_("181A"), True
        )
        self.service.setCharacteristics_([self.characteristic])
        manager.addService_(self.service)

    def peripheralManager_didAddService_error_(self, manager, service, error):
        if error:
            raise RuntimeError(str(error))
        manager.startAdvertising_(
            {
                CB.CBAdvertisementDataLocalNameKey: "YA-QA-Sensor",
                CB.CBAdvertisementDataServiceUUIDsKey: [
                    CB.CBUUID.UUIDWithString_("181A")
                ],
            }
        )
        print("FIXTURE_ADVERTISING", flush=True)

    def peripheralManagerDidStartAdvertising_error_(self, manager, error):
        print("FIXTURE_ADVERTISING_RESULT", str(error) if error else "OK", flush=True)

    def peripheralManager_didReceiveReadRequest_(self, manager, request):
        value = b"\xca\x08"
        offset = request.offset()
        if offset > len(value):
            manager.respondToRequest_withResult_(request, CB.CBATTErrorInvalidOffset)
            return
        payload = value[offset:]
        request.setValue_(NSData.dataWithBytes_length_(payload, len(payload)))
        manager.respondToRequest_withResult_(request, CB.CBATTErrorSuccess)
        print("FIXTURE_READ", flush=True)

    def peripheralManager_central_didSubscribeToCharacteristic_(
        self, manager, central, characteristic
    ):
        print("FIXTURE_SUBSCRIBED", flush=True)


delegate = Peripheral.alloc().init()
manager = CB.CBPeripheralManager.alloc().initWithDelegate_queue_options_(
    delegate, None, None
)
last = time.monotonic()
try:
    while True:
        NSRunLoop.currentRunLoop().runUntilDate_(
            NSDate.dateWithTimeIntervalSinceNow_(0.1)
        )
        if time.monotonic() - last > 1 and hasattr(delegate, "characteristic"):
            value = NSData.dataWithBytes_length_(b"\xca\x08", 2)
            manager.updateValue_forCharacteristic_onSubscribedCentrals_(
                value, delegate.characteristic, None
            )
            last = time.monotonic()
finally:
    manager.stopAdvertising()
    manager.removeAllServices()
