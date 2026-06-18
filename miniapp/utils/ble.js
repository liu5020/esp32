const {
  BLE_CONFIG_WRITE_UUID,
  BLE_SERVICE_UUID,
  BLE_STATUS_NOTIFY_UUID,
} = require('./constants');

function wxp(name, options) {
  return new Promise((resolve, reject) => {
    wx[name]({
      ...(options || {}),
      success: resolve,
      fail: reject,
    });
  });
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function normalizeUuid(value) {
  return String(value || '').toLowerCase();
}

function stringToArrayBuffer(value) {
  const encoded = unescape(encodeURIComponent(value));
  const buffer = new ArrayBuffer(encoded.length);
  const view = new Uint8Array(buffer);
  for (let i = 0; i < encoded.length; i += 1) {
    view[i] = encoded.charCodeAt(i);
  }
  return buffer;
}

function arrayBufferToString(buffer) {
  const view = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < view.length; i += 1) {
    binary += String.fromCharCode(view[i]);
  }
  return decodeURIComponent(escape(binary));
}

async function openAdapter() {
  try {
    await wxp('openBluetoothAdapter');
  } catch (error) {
    if (error && error.errCode === 10001) {
      throw new Error('请先打开手机蓝牙');
    }
    throw error;
  }
}

async function scanDevices(options) {
  const timeoutMs = (options && options.timeoutMs) || 5000;
  const keyword = String((options && options.keyword) || '').toLowerCase();
  await openAdapter();
  await wxp('startBluetoothDevicesDiscovery', {
    allowDuplicatesKey: false,
  });
  await delay(timeoutMs);
  await wxp('stopBluetoothDevicesDiscovery');
  const result = await wxp('getBluetoothDevices');
  const devices = result.devices || [];
  return devices.filter((device) => {
    const name = String(device.localName || device.name || '').toLowerCase();
    if (!keyword) {
      return name;
    }
    return name.includes(keyword);
  });
}

async function connectDevice(deviceId) {
  await wxp('createBLEConnection', { deviceId });
  const servicesResult = await wxp('getBLEDeviceServices', { deviceId });
  const service = (servicesResult.services || []).find(
    (item) => normalizeUuid(item.uuid) === normalizeUuid(BLE_SERVICE_UUID),
  );
  if (!service) {
    throw new Error('没有找到语音画纸配置服务');
  }

  const charsResult = await wxp('getBLEDeviceCharacteristics', {
    deviceId,
    serviceId: service.uuid,
  });
  const characteristics = charsResult.characteristics || [];
  const writeChar = characteristics.find(
    (item) => normalizeUuid(item.uuid) === normalizeUuid(BLE_CONFIG_WRITE_UUID),
  );
  const notifyChar = characteristics.find(
    (item) => normalizeUuid(item.uuid) === normalizeUuid(BLE_STATUS_NOTIFY_UUID),
  );
  if (!writeChar) {
    throw new Error('没有找到配置写入通道');
  }

  if (notifyChar) {
    await wxp('notifyBLECharacteristicValueChange', {
      deviceId,
      serviceId: service.uuid,
      characteristicId: notifyChar.uuid,
      state: true,
    });
  }

  return {
    deviceId,
    serviceId: service.uuid,
    writeId: writeChar.uuid,
    notifyId: notifyChar ? notifyChar.uuid : '',
  };
}

function onStatus(callback) {
  wx.onBLECharacteristicValueChange((event) => {
    try {
      callback(JSON.parse(arrayBufferToString(event.value)), event);
    } catch (error) {
      callback({ raw: arrayBufferToString(event.value) }, event);
    }
  });
}

async function writeJson(connection, payload) {
  if (!connection || !connection.deviceId || !connection.writeId) {
    throw new Error('还没有连接设备');
  }
  const message = `${JSON.stringify({
    version: 1,
    type: 'device_config',
    id: Date.now().toString(36),
    payload,
  })}\n`;
  const encoded = unescape(encodeURIComponent(message));
  const chunkSize = 20;
  for (let offset = 0; offset < encoded.length; offset += chunkSize) {
    const part = encoded.slice(offset, offset + chunkSize);
    await wxp('writeBLECharacteristicValue', {
      deviceId: connection.deviceId,
      serviceId: connection.serviceId,
      characteristicId: connection.writeId,
      value: stringToArrayBuffer(part),
    });
    await delay(30);
  }
}

module.exports = {
  connectDevice,
  onStatus,
  scanDevices,
  writeJson,
};
