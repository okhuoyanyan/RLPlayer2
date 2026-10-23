// mock_upnp_renderer.js — 本地假 UPnP MediaRenderer（自动化测试用；不打开音频设备、不出声）
// 用法：node tools/mock_upnp_renderer.js [port]   （默认 49199；GET /__state 可查调用记录）
// 提供 description.xml + AVTransport/ConnectionManager 控制面；
// 只记录 SOAP 调用，不获取音频流、不出声。
const http = require('http');
const PORT = parseInt(process.argv[2] || '49199', 10);
const state = { transport: 'STOPPED', uri: '', positionSec: 0, duration: '00:00:10', calls: [] };

const description = () => '<?xml version="1.0"?>' +
  '<root xmlns="urn:schemas-upnp-org:device-1-0">' +
  '<specVersion><major>1</major><minor>0</minor></specVersion>' +
  '<device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>' +
  '<friendlyName>Mock-N1000-Renderer</friendlyName><manufacturer>RLPlayer</manufacturer>' +
  '<modelName>RLPlayer Mock Renderer</modelName>' +
  '<UDN>uuid:mock-0000-1111-2222-333344445555</UDN>' +
  '<serviceList>' +
  '<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>' +
  '<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>' +
  '<controlURL>/ctl/avt</controlURL><eventSubURL>/evt/avt</eventSubURL><SCPDURL>/avt.xml</SCPDURL></service>' +
  '<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>' +
  '<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>' +
  '<controlURL>/ctl/cm</controlURL><eventSubURL>/evt/cm</eventSubURL><SCPDURL>/cm.xml</SCPDURL></service>' +
  '</serviceList></device></root>';

const envelope = (action, inner) => '<?xml version="1.0"?>' +
  '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" ' +
  's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>' +
  '<u:' + action + 'Response xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">' + inner +
  '</u:' + action + 'Response></s:Body></s:Envelope>';

const fmt = (s) => {
  s = Math.max(0, Math.floor(s));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return [h, m, sec].map((v) => (v < 10 ? '0' : '') + v).join(':');
};

const srv = http.createServer((req, res) => {
  let body = '';
  req.on('data', (c) => { body += c; });
  req.on('end', () => {
    if (req.method === 'GET' && req.url.indexOf('/description.xml') === 0) {
      res.writeHead(200, { 'Content-Type': 'text/xml; charset="utf-8"' });
      return res.end(description());
    }
    if (req.method === 'GET' && req.url === '/__state') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      return res.end(JSON.stringify(state));
    }
    if (req.method !== 'POST') { res.writeHead(404); return res.end(); }

    const raw = String(req.headers['soapaction'] || '');
    const mm = /#([A-Za-z]+)/.exec(raw);
    const action = mm ? mm[1] : '';
    if (action === 'SetAVTransportURI') {
      const u = /<CurrentURI>([^<]*)<\/CurrentURI>/.exec(body);
      state.uri = u ? u[1] : '';
      state.transport = 'STOPPED';
      state.positionSec = 0;
    } else if (action === 'Play') {
      state.transport = 'PLAYING';
    } else if (action === 'Pause') {
      state.transport = 'PAUSED_PLAYBACK';
    } else if (action === 'Stop') {
      state.transport = 'STOPPED';
    } else if (action === 'Seek') {
      const t = /<Target>([^<]*)<\/Target>/.exec(body);
      if (t) {
        const p = t[1].split(':');
        state.positionSec = (parseInt(p[0], 10) || 0) * 3600 + (parseInt(p[1], 10) || 0) * 60 + (parseInt(p[2], 10) || 0);
      }
    }
    state.calls.push({ action, transport: state.transport, uri: state.uri, body: body.slice(0, 300) });

    let inner = '';
    if (action === 'GetTransportInfo') {
      inner = '<CurrentTransportState>' + state.transport + '</CurrentTransportState>' +
              '<CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>';
    } else if (action === 'GetPositionInfo') {
      inner = '<Track>1</Track><TrackDuration>' + state.duration + '</TrackDuration>' +
              '<TrackMetaData></TrackMetaData><TrackURI>' + state.uri + '</TrackURI>' +
              '<RelTime>' + fmt(state.positionSec) + '</RelTime><AbsTime>' + fmt(state.positionSec) + '</AbsTime>' +
              '<RelCount>0</RelCount><AbsCount>0</AbsCount>';
    } else if (action === 'GetMediaInfo') {
      inner = '<NrTracks>1</NrTracks><MediaDuration>' + state.duration + '</MediaDuration>' +
              '<CurrentURI>' + state.uri + '</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>' +
              '<NextURI></NextURI><NextURIMetaData></NextURIMetaData><PlayMedium>NETWORK</PlayMedium>' +
              '<RecordMedium>NOT_IMPLEMENTED</RecordMedium><WriteStatus>NOT_IMPLEMENTED</WriteStatus>';
    } else if (action === 'GetProtocolInfo') {
      inner = '<Source></Source><Sink>http-get:*:audio/wav:*</Sink>';
    }
    res.writeHead(200, { 'Content-Type': 'text/xml; charset="utf-8"' });
    res.end(envelope(action, inner));
  });
});
srv.listen(PORT, '127.0.0.1', () => console.log('mock renderer on http://127.0.0.1:' + PORT + '/'));
