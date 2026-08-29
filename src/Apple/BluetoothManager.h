// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

/*
 * Private Objective-C interfaces shared by BluetoothHelper.mm and
 * BleSerialPort.mm.  Not for inclusion by C++ code.
 */

#pragma once

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include "DetectDeviceListener.hpp"
#include "Device/Port/State.hpp"

#include <cstddef>
#include <span>

class PortListener;
class DataHandler;

@class XCSBleSerialPort;

/**
 * dispatch_queue_set_specific() key identifying the manager's queue,
 * so code can tell whether it already runs on it.
 */
extern void *_Nonnull const XCSBluetoothQueueKey;

NS_ASSUME_NONNULL_BEGIN

/**
 * Owns the CBCentralManager and its private dispatch queue, performs
 * scanning and routes connection events to XCSBleSerialPort objects.
 */
@interface XCSBluetoothManager : NSObject <CBCentralManagerDelegate>

@property(nonatomic, readonly) dispatch_queue_t queue;
@property(nonatomic, readonly) CBCentralManager *central;

/* thread-safe */
- (BOOL)isPoweredOn;
- (BOOL)isUnsupported;
- (nullable NSString *)nameForIdentifier:(NSString *)identifier;
- (void)addDetectDeviceListener:(DetectDeviceListener *)listener;
- (void)removeDetectDeviceListener:(DetectDeviceListener *)listener;

/**
 * Create a serial port object for the given peripheral identifier
 * (or, if that is not a UUID, the advertised device name) and start
 * connecting.  Thread-safe.
 */
- (XCSBleSerialPort *)openBleSerialPort:(NSString *)address;

/* the following are only called on the manager's queue */
- (void)portClosed:(XCSBleSerialPort *)port;
- (void)updateScanning;

@end

/**
 * One BLE serial connection (Nordic UART Service, Microchip/ISSC
 * transparent UART or HM-10).  Implements chunked, flow-controlled
 * writes and automatic reconnection.
 */
@interface XCSBleSerialPort : NSObject <CBPeripheralDelegate>

@property(nonatomic, readonly, nullable) NSUUID *identifier;
@property(nonatomic, readonly, nullable) NSString *targetName;
@property(nonatomic, readonly, nullable) CBPeripheral *peripheral;

- (instancetype)initWithManager:(XCSBluetoothManager *)manager
                     identifier:(nullable NSUUID *)identifier
                     targetName:(nullable NSString *)targetName;

/* thread-safe; see PortBridge */
- (void)setListener:(nullable PortListener *)listener;
- (void)setInputListener:(nullable DataHandler *)handler;
- (PortState)state;
- (std::size_t)write:(std::span<const std::byte>)src;
- (BOOL)drain;
- (void)close;

/* the following are only called on the manager's queue */
- (void)start;
- (void)managerPoweredOn;
- (void)managerPoweredOff;
- (BOOL)needsScan;
- (BOOL)matchesDiscoveredPeripheral:(CBPeripheral *)peripheral
                               name:(nullable NSString *)name;
- (void)attachDiscoveredPeripheral:(CBPeripheral *)peripheral;
- (void)didConnect;
- (void)didFailToConnect:(nullable NSError *)error;
- (void)didDisconnect:(nullable NSError *)error;

@end

NS_ASSUME_NONNULL_END
