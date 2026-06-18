const { loadConfig } = require('./storage');

function trimSlash(value) {
  return String(value || '').replace(/\/+$/, '');
}

function encodeQuery(params) {
  if (!params) {
    return '';
  }
  const pairs = Object.keys(params)
    .filter((key) => params[key] !== undefined && params[key] !== null)
    .map((key) => `${encodeURIComponent(key)}=${encodeURIComponent(params[key])}`);
  return pairs.length ? `?${pairs.join('&')}` : '';
}

function buildUrl(path, params, baseUrl) {
  const config = loadConfig();
  const base = trimSlash(baseUrl || config.backend.baseUrl);
  const cleanPath = path.startsWith('/') ? path : `/${path}`;
  return `${base}${cleanPath}${encodeQuery(params)}`;
}

function request(options) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: options.url,
      method: options.method || 'GET',
      data: options.data,
      header: options.header || {},
      timeout: options.timeout || 20000,
      success(res) {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve(res.data);
          return;
        }
        reject({
          statusCode: res.statusCode,
          data: res.data,
        });
      },
      fail: reject,
    });
  });
}

function getHealth(baseUrl) {
  return request({
    url: buildUrl('/health', null, baseUrl),
    timeout: 8000,
  });
}

function draw(text, baseUrl) {
  return request({
    url: buildUrl('/draw', { text }, baseUrl),
    timeout: 90000,
  });
}

function getBackendConfig(baseUrl) {
  return request({
    url: buildUrl('/config', null, baseUrl),
    timeout: 8000,
  });
}

function updateBackendConfig(payload, baseUrl) {
  return request({
    url: buildUrl('/config', null, baseUrl),
    method: 'POST',
    data: payload,
    header: {
      'content-type': 'application/json',
    },
    timeout: 12000,
  });
}

function getLatest(baseUrl) {
  return request({
    url: buildUrl('/latest', null, baseUrl),
    timeout: 8000,
  });
}

module.exports = {
  buildUrl,
  draw,
  getBackendConfig,
  getHealth,
  getLatest,
  updateBackendConfig,
};
