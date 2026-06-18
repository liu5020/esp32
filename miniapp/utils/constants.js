const BLE_SERVICE_UUID = '7a8f0001-7d2a-4f2c-8f9d-0a1b2c3d4e5f';
const BLE_CONFIG_WRITE_UUID = '7a8f0002-7d2a-4f2c-8f9d-0a1b2c3d4e5f';
const BLE_STATUS_NOTIFY_UUID = '7a8f0003-7d2a-4f2c-8f9d-0a1b2c3d4e5f';

const AI_MODES = [
  {
    id: 'local_first',
    label: '本地识别优先',
    description: '香橙派本地 STT，失败后走云端，图片用百炼。',
    sttProvider: 'local_sherpa_onnx',
    sttFallbackProvider: 'dashscope_paraformer',
    imageProvider: 'aliyun_wanx',
  },
  {
    id: 'cloud_all',
    label: '全云端模式',
    description: '语音识别和图片生成都走云端，适合本地模型不可用时。',
    sttProvider: 'dashscope_paraformer',
    sttFallbackProvider: '',
    imageProvider: 'aliyun_wanx',
  },
  {
    id: 'custom_backend',
    label: '自定义后端',
    description: 'ESP32 只对接你指定的兼容 HTTP 服务。',
    sttProvider: 'custom_backend',
    sttFallbackProvider: '',
    imageProvider: 'custom_backend',
  },
];

const DEFAULT_CONFIG = {
  device: {
    nameKeyword: 'VoiceSketch',
    wifiSsid: '',
    wifiPassword: '',
  },
  backend: {
    baseUrl: 'http://192.168.31.58:8787',
    aiMode: 'local_first',
    sttProvider: 'local_sherpa_onnx',
    sttFallbackProvider: 'dashscope_paraformer',
    imageProvider: 'aliyun_wanx',
    imageModel: 'wan2.2-t2i-flash',
  },
  advanced: {
    sttPath: '/stt',
    drawPath: '/draw',
    healthPath: '/health',
    latestPath: '/latest',
    imageThreshold: 210,
    printEnabled: false,
  },
};

module.exports = {
  AI_MODES,
  BLE_CONFIG_WRITE_UUID,
  BLE_SERVICE_UUID,
  BLE_STATUS_NOTIFY_UUID,
  DEFAULT_CONFIG,
};
