#!/usr/bin/env python3
"""
Интерактивное превью «Станция отношений» v1.3.8:
- Круглый экран GC9A01A (5 страниц с плавными переходами по Кнопке 2: Таймер → Открытка → Текст → Погода → IP/Сердце)
- Поддержка полноэкранных открыток LVC1 на 2-й странице и отдельного текстового сообщения на 3-й странице
- 4-я страница погоды с «ГУЛЯБЕЛЬНО ❤», 5-я страница — информация о сайте
- Удалённый конструктор открыток/сообщений (/sender) и сайт станции (/site)
"""

import gzip
import json
import os
import re
import struct
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_UI_H = os.path.join(ROOT_DIR, "love_gift_esp32c6", "web_ui.h")
SENDER_HTML = os.path.join(ROOT_DIR, "sender", "index.html")

CARDS = [None, None, None, None, None]

START_BOOT_TS = time.time()

STATE = {
    "connected": True,
    "ssid": "Home_WiFi_5G",
    "saved_ssid": "Home_WiFi_5G",
    "saved_pass": "krasnoyarsk_2026",
    "has_saved_pass": True,
    "ip": "192.168.1.48",
    "tz_offset": 7,
    "rotation": 0,
    "version": "1.3.8",
    "commit": "95980b7",
    "ota_target_commit": "95980b7",
    "pulse_seq": 0,
    "ota_seq": 0,
    "msg_seq": 0,
    "plain_seq": 0,
    "status_seq": 0,
    "my_status": {
        "icon": 0,
        "text": "Думаю о тебе ♥",
        "epoch": int(time.time()) - 120,
    },
    "unread": False,
    "plain_unread": False,
    "plain_text": "Люблю тебя без повода ❤️",
    "plain_time": int(time.time()) - 300,
    "weather": {
        "valid": True,
        "city": "Красноярск",
        "temp": -6,
        "feels": -10,
        "wind": 3,
        "code": 2,
        "desc": "Переменная облачность",
        "walkable": True,
    },
    "messages": [
        {
            "text": "Я всегда рядом с тобой ❤️",
            "time": int(time.time()) - 600,
            "has_card": False,
        }
    ],
    "replies": [],
}

MOCK_NETWORKS = [
    {"ssid": "Home_WiFi_5G", "rssi": -48, "open": False},
    {"ssid": "Kvartira_42", "rssi": -61, "open": False},
    {"ssid": "Keenetic-7814", "rssi": -73, "open": False},
    {"ssid": "iPhone", "rssi": -54, "open": False},
]


def load_esp_html() -> str:
    with open(WEB_UI_H, "r", encoding="utf-8") as f:
        raw = f.read()
    m_gz = re.search(r'WEB_HTML_GZ\[\]\s*PROGMEM\s*=\s*\{(.*?)\};', raw, re.DOTALL)
    if m_gz:
        vals = bytes(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m_gz.group(1)))
        return gzip.decompress(vals).decode("utf-8")
    m = re.search(r'R"rawliteral\((.*)\)rawliteral"', raw, re.DOTALL)
    return m.group(1).strip() if m else "<h1>web_ui.h parse error</h1>"


def load_sender_html() -> str:
    with open(SENDER_HTML, "r", encoding="utf-8") as f:
        return f.read()


STUDIO_HTML = r"""<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Станция отношений v1.2.0 — Живой стенд</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: radial-gradient(circle at top, #23151f 0%, #110a10 100%);
      color: #fce7f3;
      min-height: 100vh;
      padding: 16px 14px 36px;
    }
    .top-bar {
      max-width: 1180px;
      margin: 0 auto 16px;
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      background: rgba(255, 255, 255, 0.05);
      border: 1px solid rgba(255, 130, 170, 0.20);
      padding: 13px 18px;
      border-radius: 20px;
    }
    .top-bar h1 { font-size: 16.5px; font-weight: 700; color: #ffd6e6; }
    .top-bar p { font-size: 12px; color: #cf9fb3; margin-top: 2px; }
    .mode-btns { display: flex; gap: 8px; flex-wrap: wrap; }
    .ctrl-btn {
      background: rgba(240, 67, 114, 0.16);
      color: #ffd6e6;
      border: 1px solid rgba(255, 110, 150, 0.36);
      padding: 8px 12px;
      border-radius: 12px;
      font-size: 12.5px;
      font-weight: 600;
      cursor: pointer;
      transition: 0.15s;
    }
    .ctrl-btn:hover { background: rgba(240, 67, 114, 0.32); }
    .ctrl-btn.active { background: #e82e61; color: #fff; border-color: #ff6b95; }

    .layout {
      max-width: 1180px;
      margin: 0 auto;
      display: grid;
      grid-template-columns: 370px 1fr;
      gap: 20px;
      align-items: start;
    }
    @media (max-width: 920px) { .layout { grid-template-columns: 1fr; } }

    .device-panel, .phone-panel {
      background: rgba(255, 255, 255, 0.045);
      border: 1px solid rgba(255, 140, 180, 0.16);
      border-radius: 28px;
      padding: 18px;
    }
    .device-panel { text-align: center; }
    .panel-tag {
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: 1px;
      color: #f4a3c8;
      margin-bottom: 12px;
      font-weight: 700;
    }
    .watch-bezel {
      width: 268px;
      height: 268px;
      margin: 0 auto;
      border-radius: 50%;
      padding: 14px;
      background: linear-gradient(145deg, #36222f, #160d13);
      box-shadow: 0 18px 45px rgba(0, 0, 0, 0.65), inset 0 2px 4px rgba(255, 255, 255, 0.14);
      display: flex;
      align-items: center;
      justify-content: center;
    }
    canvas#tft {
      width: 240px;
      height: 240px;
      border-radius: 50%;
      background: #000;
    }
    .hw-buttons {
      display: flex;
      justify-content: center;
      gap: 12px;
      margin-top: 16px;
    }
    .hw-btn {
      flex: 1;
      padding: 11px 10px;
      border-radius: 14px;
      border: 1px solid rgba(255, 130, 170, 0.32);
      background: linear-gradient(180deg, #311b27 0%, #201019 100%);
      color: #ffe4ef;
      font-weight: 700;
      font-size: 13px;
      cursor: pointer;
      user-select: none;
    }
    .hw-btn:active { transform: scale(0.96); background: #e82e61; }
    .hw-hint {
      margin-top: 14px;
      font-size: 12px;
      line-height: 1.55;
      color: #c79fb1;
      text-align: left;
      background: rgba(0,0,0,0.26);
      padding: 11px 13px;
      border-radius: 14px;
    }
    .phone-switch {
      display: flex;
      justify-content: center;
      gap: 8px;
      margin-bottom: 12px;
      flex-wrap: wrap;
    }
    .phone-frame {
      max-width: 470px;
      margin: 0 auto;
      border-radius: 34px;
      padding: 10px;
      background: #1c1118;
      border: 2px solid #442939;
      box-shadow: 0 20px 50px rgba(0,0,0,0.5);
    }
    iframe {
      width: 100%;
      height: 720px;
      border: none;
      border-radius: 26px;
      background: #180e16;
      display: block;
    }
  </style>
</head>
<body>
  <div class="top-bar">
    <div>
      <h1>Станция отношений v1.3.8 — 5 страниц + Красноярск + Удалённая админка</h1>
      <p>Нажимай Кнопку 2 для листания 5 страниц, задерживай Кнопку 1 на 10 сек для ответного сердечка</p>
    </div>
    <div class="mode-btns">
      <button class="ctrl-btn" onclick="forceApMode()">📡 1-й запуск ("love")</button>
      <button class="ctrl-btn" onclick="forceConnectedMode()">🏠 Уже дома</button>
      <button class="ctrl-btn" id="bigDaysBtn" onclick="toggleBigDaysTest()">⏩ +10 лет</button>
      <button class="ctrl-btn" onclick="startSimOta()">🚀 Тест OTA</button>
    </div>
  </div>

  <div class="layout">
    <div class="device-panel">
      <div class="panel-tag">Круглый экран 240×240 (GC9A01A)</div>
      <div class="watch-bezel">
        <canvas id="tft" width="240" height="240"></canvas>
      </div>
      <div class="hw-buttons">
        <button class="hw-btn" onmousedown="btn1Down(event)" onmouseup="btn1Up(event)" onmouseleave="btn1Up(event)" ontouchstart="btn1Down(event)" ontouchend="btn1Up(event)" ontouchcancel="btn1Up(event)">❤️ Кнопка 1 (D0)<br><span style="font-weight:400;font-size:11px;opacity:0.8;">Зажми на 10 сек — ♥</span></button>
        <button class="hw-btn" onmousedown="btn2Down(event)" onmouseup="btn2Up(event)" onmouseleave="btn2Up(event)" ontouchstart="btn2Down(event)" ontouchend="btn2Up(event)" ontouchcancel="btn2Up(event)">🔄 Кнопка 2 (D1)<br><span style="font-weight:400;font-size:11px;opacity:0.8;">Листание 6 страниц</span></button>
      </div>
      <div class="hw-hint">
        • <b>Зажми Кнопку 1 (D0) на 10 сек</b> — увидишь таймер удержания и переливающийся прогресс-бар отправки сердечка (отпустишь раньше — отмена).<br>
        • Нажми <b>Кнопку 2 (D1)</b> — листай 5 страниц: Таймер → Открытка → Текст → Погода → IP/Сердце.<br>
        • Справа открыт <b>🎨 Конструктор открыток + Админка</b> или <b>📱 Сайт станции</b>!
      </div>
    </div>

    <div class="phone-panel">
      <div class="phone-switch">
        <button class="ctrl-btn active" id="viewSenderBtn" onclick="switchRightView('sender')">🎨 Твой конструктор открыток (издалека)</button>
        <button class="ctrl-btn" id="viewGirlBtn" onclick="switchRightView('site')">📱 Сайт станции (у неё дома)</button>
      </div>
      <div class="phone-frame">
        <iframe id="siteFrame" src="/sender"></iframe>
      </div>
    </div>
  </div>

  <script>
    const canvas = document.getElementById('tft');
    const ctx = canvas.getContext('2d');

    const tickerOff = document.createElement('canvas');
    tickerOff.width = 152; tickerOff.height = 16;
    const tctx = tickerOff.getContext('2d');

    const msgOff = document.createElement('canvas');
    msgOff.width = 172; msgOff.height = 86;
    const mctx = msgOff.getContext('2d');

    const cardOff = document.createElement('canvas');
    cardOff.width = 240; cardOff.height = 240;
    const cctx = cardOff.getContext('2d');
    let hasDecodedCard = false;

    const prevFrameOff = document.createElement('canvas');
    prevFrameOff.width = 240; prevFrameOff.height = 240;
    const pctx = prevFrameOff.getContext('2d');

    let state = {
      connected: true, ssid: 'Home_WiFi_5G', ip: '192.168.1.48', tz_offset: 7,
      version: '1.3.8', pulse_seq: 0, ota_seq: 0, msg_seq: 0, plain_seq: 0, unread: false, plain_unread: false,
      plain_text: 'Люблю тебя без повода ❤️', plain_time: 1795785300,
      weather: { valid: true, city: 'Красноярск', temp: -6, feels: -10, wind: 3, code: 2, desc: 'Переменная облачность', walkable: true },
      messages: [{ text: 'Я всегда рядом с тобой ❤️', time: 1795785000, has_card: false }]
    };
    let lastPulseSeq = 0, lastOtaSeq = 0, lastMsgSeq = 0, lastPlainSeq = 0, lastStatusSeq = 0;
    let screenMode = 'timer'; // 'ap' | 'timer' | 'messages' | 'text_msg' | 'weather' | 'my_status' | 'heart_ip'
    let pulseUntil = 0;
    let pulseReplyShown = false;
    let bannerUntil = 0;
    let otaSimActive = false, otaSimStart = 0, otaTargetVer = '1.2.1';
    let phraseIdx = 0;
    let extraSecondsTest = 0;
    let tickerScrollPx = 0;
    let msgScrollY = 0, msgScrollDir = 1, msgPauseSec = 1.8;
    let lastMsgRenderedText = '';
    let lastRenderTs = performance.now();

    // Состояние анимации переключения страниц (как в прошивке ESP32!)
    let transActive = false;
    let transPhase = 0;
    let transTarget = 'timer';
    let transStartMs = 0;

    const phrases = ['ЛЮБЛЮ ТЕБЯ', 'ВСЕГДА РЯДОМ', 'ТЫ ЛУЧШАЯ', 'НАШЕ ВРЕМЯ', 'КРЕПКО ОБНИМАЮ', 'МОЯ РАДОСТЬ'];
    const BTN1_HEART_HOLD_MS = 10000;
    const BTN1_HOLD_UI_DELAY_MS = 120;
    let b1DownAt = 0;
    let b1HoldCompleted = false;
    let b2DownAt = 0;

    const SHIMMER_PALETTE = [
      [255, 120, 175], [255, 42, 102], [255, 92, 76], [255, 188, 118], [218, 96, 255]
    ];

    function switchRightView(which) {
      document.getElementById('viewGirlBtn').classList.toggle('active', which === 'site');
      document.getElementById('viewSenderBtn').classList.toggle('active', which === 'sender');
      document.getElementById('siteFrame').src = (which === 'sender') ? '/sender' : '/site';
    }

    function decodeLvcToCardOff(buf) {
      const u8 = new Uint8Array(buf);
      if (u8.length < 70 || u8[0] !== 76 || u8[1] !== 86 || u8[2] !== 67 || u8[3] !== 49) return false;
      const dv = new DataView(buf);
      const capLen = dv.getUint16(8, true);
      const stripLens = [];
      for (let i = 0; i < 15; i++) stripLens.push(dv.getUint32(10 + i * 4, true));

      const img = cctx.createImageData(240, 240);
      const d = img.data;
      let offset = 70 + capLen;
      let outPix = 0;

      for (let s = 0; s < 15; s++) {
        const end = offset + stripLens[s];
        const index = new Uint16Array(64);
        let prev = 0, p = offset, stripLeft = 240 * 16;

        while (stripLeft > 0) {
          let b1 = p < end ? u8[p++] : 0x40;
          if (b1 === 0xFE) {
            prev = ((u8[p] << 8) | u8[p + 1]) & 0xFFFF;
            p += 2;
          } else if ((b1 & 0xC0) === 0x40) {
            let run = (b1 & 0x3F) + 1;
            const r = ((prev >> 11) & 0x1F) * 255 / 31;
            const g = ((prev >> 5)  & 0x3F) * 255 / 63;
            const b = ( prev        & 0x1F) * 255 / 31;
            while (run-- > 0 && stripLeft > 0) {
              const o = outPix * 4;
              d[o] = r; d[o+1] = g; d[o+2] = b; d[o+3] = 255;
              outPix++; stripLeft--;
            }
            continue;
          } else if ((b1 & 0xC0) === 0x00) {
            prev = index[b1 & 0x3F];
          } else if ((b1 & 0xC0) === 0x80) {
            const pr = (prev >> 11) & 0x1F, pg = (prev >> 5) & 0x3F, pb = prev & 0x1F;
            const nr = (pr + ((b1 >> 4) & 3) - 2) & 0x1F;
            const ng = (pg + ((b1 >> 2) & 3) - 2) & 0x3F;
            const nb = (pb + ( b1       & 3) - 2) & 0x1F;
            prev = (nr << 11) | (ng << 5) | nb;
          } else {
            const b2 = u8[p++];
            const pr = (prev >> 11) & 0x1F, pg = (prev >> 5) & 0x3F, pb = prev & 0x1F;
            const dg = (b1 & 0x1F) - 16;
            const dg2 = Math.trunc(dg / 2);
            const nr = (pr + dg2 + (((b2 >> 4) & 0x0F) - 8)) & 0x1F;
            const ng = (pg + dg) & 0x3F;
            const nb = (pb + dg2 + (( b2       & 0x0F) - 8)) & 0x1F;
            prev = (nr << 11) | (ng << 5) | nb;
          }
          const r5 = (prev >> 11) & 0x1F, g6 = (prev >> 5) & 0x3F, b5 = prev & 0x1F;
          index[(r5 * 3 + g6 * 5 + b5 * 7) & 63] = prev;
          const o = outPix * 4;
          d[o] = r5 * 255 / 31; d[o+1] = g6 * 255 / 63; d[o+2] = b5 * 255 / 31; d[o+3] = 255;
          outPix++; stripLeft--;
        }
        offset = end;
      }
      cctx.putImageData(img, 0, 0);
      hasDecodedCard = true;
      return true;
    }

    async function fetchLatestCardIfAny() {
      const m = state.messages && state.messages[0];
      if (!m || !m.has_card) { hasDecodedCard = false; return; }
      try {
        const res = await fetch('/api/card?idx=0&t=' + state.msg_seq);
        if (res.ok) {
          decodeLvcToCardOff(await res.arrayBuffer());
        }
      } catch (e) {}
    }

    function evalShimmerRGB(phase) {
      let p = phase - Math.floor(phase);
      let scaled = p * 5.0;
      let idx0 = Math.floor(scaled) % 5;
      let idx1 = (idx0 + 1) % 5;
      let t = 0.5 - 0.5 * Math.cos((scaled - Math.floor(scaled)) * Math.PI);
      return [
        Math.round(SHIMMER_PALETTE[idx0][0] + (SHIMMER_PALETTE[idx1][0] - SHIMMER_PALETTE[idx0][0]) * t),
        Math.round(SHIMMER_PALETTE[idx0][1] + (SHIMMER_PALETTE[idx1][1] - SHIMMER_PALETTE[idx0][1]) * t),
        Math.round(SHIMMER_PALETTE[idx0][2] + (SHIMMER_PALETTE[idx1][2] - SHIMMER_PALETTE[idx0][2]) * t)
      ];
    }

    function pluralUpper(n, one, two, five) {
      let n100 = n % 100, n10 = n % 10;
      if (n100 > 10 && n100 < 20) return five;
      if (n10 === 1) return one;
      if (n10 >= 2 && n10 <= 4) return two;
      return five;
    }

    function groupSpaces(n) {
      return Math.floor(n).toString().replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
    }

    function getBeatScale(ms) {
      const phase = (ms % 1400) / 1400;
      if (phase < 0.12) return 0.92 + 0.15 * Math.sin((phase / 0.12) * Math.PI);
      if (phase >= 0.18 && phase < 0.32) return 0.92 + 0.09 * Math.sin(((phase - 0.18) / 0.14) * Math.PI);
      return 0.92;
    }

    function draw3DHeart(cx, cy, scale, shimmer = false, nowMs = 0) {
      const W = 112, H = 100;
      const img = ctx.createImageData(W, H);
      const data = img.data;
      const inv = 1 / Math.max(0.12, scale);
      let baseRGB = shimmer ? evalShimmerRGB(nowMs * 0.00028) : [255, 42, 98];

      for (let py = 0; py < H; py++) {
        const ym = (1.22 - (py / (H - 1)) * 2.40) * inv;
        const ym2 = ym * ym, ym3 = ym2 * ym;
        for (let px = 0; px < W; px++) {
          const xm = (((px / (W - 1)) * 2.56) - 1.28) * inv;
          const xm2 = xm * xm;
          const a = xm2 + ym2 - 1.0;
          if (a * a * a - xm2 * ym3 <= 0) {
            let shade = Math.max(0.35, Math.min(1.0, 0.68 + 0.22 * ym - 0.14 * xm));
            const hl = (xm + 0.42) ** 2 + (ym - 0.46) ** 2;
            let r, g, b;
            if (hl < 0.045) { r = 255; g = 228; b = 238; }
            else if (hl < 0.11) { r = 255; g = 150; b = 185; }
            else { r = baseRGB[0] * shade; g = baseRGB[1] * shade + 14; b = baseRGB[2] * shade + 22; }
            const idx = (py * W + px) * 4;
            data[idx] = r; data[idx + 1] = g; data[idx + 2] = b; data[idx + 3] = 255;
          }
        }
      }
      ctx.putImageData(img, cx - W / 2, cy - H / 2);
    }

    function drawMiniHeartSmooth(cx, cy, ms, shimmer = false) {
      const s = getBeatScale(ms);
      const r = 8.2 * s;
      if (shimmer) {
        const [cr, cg, cb] = evalShimmerRGB(ms * 0.0004);
        ctx.fillStyle = `rgb(${cr},${cg},${cb})`;
      } else {
        ctx.fillStyle = '#ff4977';
      }
      ctx.beginPath();
      ctx.arc(cx - r * 0.5, cy - r * 0.3, r * 0.58, 0, Math.PI * 2);
      ctx.arc(cx + r * 0.5, cy - r * 0.3, r * 0.58, 0, Math.PI * 2);
      ctx.fill();
      ctx.beginPath();
      ctx.moveTo(cx - r * 1.04, cy - r * 0.1);
      ctx.lineTo(cx + r * 1.04, cy - r * 0.1);
      ctx.lineTo(cx, cy + r * 1.08);
      ctx.closePath();
      ctx.fill();
    }

    function drawOrbitShimmerHeart(exactSec0to60, nowMs) {
      const S = 18, R_ORBIT = 110.0;
      const ang = (exactSec0to60 * 6.0 - 90.0) * Math.PI / 180.0;
      const fx = 120.0 + Math.cos(ang) * R_ORBIT;
      const fy = 120.0 + Math.sin(ang) * R_ORBIT;
      const bx = Math.round(fx) - 9, by = Math.round(fy) - 9;
      const cxLocal = fx - bx, cyLocal = fy - by;
      const pulse = 1.0 + 0.06 * Math.sin(nowMs * 0.007);
      const invR = 1.0 / (5.1 * pulse);
      const basePhase = nowMs * 0.00032;

      const patch = ctx.getImageData(bx, by, S, S);
      const d = patch.data;
      for (let py = 0; py < S; py++) {
        const ym = (cyLocal - (py + 0.5)) * invR + 0.10;
        const ym2 = ym * ym, ym3 = ym2 * ym;
        for (let px = 0; px < S; px++) {
          const xm = ((px + 0.5) - cxLocal) * invR;
          const xm2 = xm * xm;
          const a = xm2 + ym2 - 1.0;
          const val = a * a * a - xm2 * ym3;
          if (val <= 0.12) {
            let [r, g, b] = evalShimmerRGB(basePhase + 0.14 * (xm - ym));
            if ((xm + 0.38) ** 2 + (ym - 0.42) ** 2 < 0.12) {
              r = (r + 255) >> 1; g = (g + 245) >> 1; b = (b + 250) >> 1;
            }
            let cov = val <= -0.14 ? 1.0 : Math.max(0, Math.min(1, (0.12 - val) / 0.26));
            if (cov > 0.12) {
              const i = (py * S + px) * 4;
              d[i]     = Math.round(d[i] * (1 - cov) + r * cov);
              d[i + 1] = Math.round(d[i + 1] * (1 - cov) + g * cov);
              d[i + 2] = Math.round(d[i + 2] * (1 - cov) + b * cov);
              d[i + 3] = 255;
            }
          }
        }
      }
      ctx.putImageData(patch, bx, by);
    }

    function drawRings() {
      ctx.strokeStyle = '#4a1830';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(120, 120, 117.5, 0, Math.PI * 2);
      ctx.stroke();
    }

    function drawPageDots(pageNum, onPhoto = false) {
      const y = onPhoto ? 225 : 207;
      if (onPhoto) {
        ctx.fillStyle = '#1f0b15';
        ctx.beginPath();
        ctx.roundRect(80, y - 5, 80, 11, 5);
        ctx.fill();
      }
      for (let i = 1; i <= 6; i++) {
        const x = 90 + (i - 1) * 12;
        ctx.beginPath();
        ctx.arc(x, y, 3, 0, Math.PI * 2);
        if (i === pageNum) {
          ctx.fillStyle = '#ff4977';
          ctx.fill();
        } else if ((i === 2 && state.unread) || (i === 3 && state.plain_unread)) {
          ctx.fillStyle = '#ffe58f';
          ctx.fill();
        } else {
          ctx.strokeStyle = '#4a1830';
          ctx.lineWidth = 1.5;
          ctx.stroke();
        }
      }
    }

    function drawMyStatusScreen(now) {
      const st = state.my_status || { icon: 0, text: 'Думаю о тебе ♥', epoch: 0 };
      const ic = (st.icon || 0) % 7;
      const accents = ['#ff4977', '#ffd466', '#55f29b', '#8cb8ff', '#ff94ae', '#ffa548', '#55f29b'];
      const accent = accents[ic];

      drawRings();
      ctx.strokeStyle = accent + '66';
      ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.arc(120, 120, 115, 0, Math.PI * 2); ctx.stroke();

      centerText('СЕЙЧАС Я', 19, 10.5, accent);

      // Анимированная иконка в зоне 92..148 x 42..86
      const ox = 92, oy = 42;
      ctx.save();
      if (ic === 0) {
        const sc = 1 + 0.14 * Math.sin(now * 0.0055);
        const r = 8.5 * sc;
        const cx = ox + 28, cy = oy + 20;
        ctx.fillStyle = accent;
        ctx.beginPath(); ctx.arc(cx - r + 2, cy - 3, r, 0, Math.PI * 2); ctx.fill();
        ctx.beginPath(); ctx.arc(cx + r - 2, cy - 3, r, 0, Math.PI * 2); ctx.fill();
        ctx.beginPath(); ctx.moveTo(cx - 2 * r + 2, cy - 1); ctx.lineTo(cx + 2 * r - 2, cy - 1); ctx.lineTo(cx, cy + 2 * r - 1); ctx.fill();
        ctx.fillStyle = '#fff'; ctx.beginPath(); ctx.arc(cx - r + 1, cy - 5, 2, 0, Math.PI * 2); ctx.fill();
      } else if (ic === 1) {
        ctx.strokeStyle = accent; ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.roundRect(ox + 10, oy + 8, 36, 22, 3); ctx.stroke();
        ctx.fillStyle = '#1f0b15'; ctx.fillRect(ox + 12, oy + 10, 32, 18);
        ctx.fillStyle = accent; ctx.beginPath(); ctx.roundRect(ox + 6, oy + 31, 44, 5, 2); ctx.fill();
        const la = Math.floor(now / 280) % 4;
        for (let i = 0; i < 3; i++) {
          ctx.fillStyle = (i === la) ? '#fff' : accent;
          ctx.fillRect(ox + 15, oy + 14 + i * 4, (i === la) ? 22 : (14 + ((i * 5) % 10)), 1.5);
        }
      } else if (ic === 2) {
        const bob = Math.sin(now * 0.015) * 1.2;
        ctx.fillStyle = accent;
        ctx.beginPath(); ctx.roundRect(ox + 14, oy + 18 + bob, 28, 11, 4); ctx.fill();
        ctx.beginPath(); ctx.roundRect(ox + 19, oy + 12 + bob, 17, 8, 3); ctx.fill();
        ctx.fillStyle = '#000';
        ctx.fillRect(ox + 21, oy + 14 + bob, 5, 4); ctx.fillRect(ox + 28, oy + 14 + bob, 6, 4);
        ctx.fillStyle = '#fff';
        ctx.beginPath(); ctx.arc(ox + 20, oy + 30, 4, 0, Math.PI * 2); ctx.fill();
        ctx.beginPath(); ctx.arc(ox + 36, oy + 30, 4, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#ffe58f'; ctx.beginPath(); ctx.arc(ox + 42, oy + 22 + bob, 2, 0, Math.PI * 2); ctx.fill();
        const rp = Math.floor(now / 65) % 12;
        ctx.fillStyle = '#4a1830';
        for (let x = -rp; x < 56; x += 12) ctx.fillRect(ox + Math.max(0, x), oy + 37, 6, 1.5);
        ctx.fillStyle = '#ff4977';
        ctx.beginPath(); ctx.arc(ox + 28, oy + 7 + bob, 3, 0, Math.PI * 2); ctx.fill();
      } else if (ic === 3) {
        ctx.fillStyle = accent; ctx.beginPath(); ctx.arc(ox + 24, oy + 22, 12, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#000'; ctx.beginPath(); ctx.arc(ox + 30, oy + 18, 10, 0, Math.PI * 2); ctx.fill();
        const ph = Math.floor(now / 350) % 3;
        ctx.fillStyle = '#fff'; ctx.beginPath(); ctx.arc(ox + 39, oy + 11, ph === 0 ? 2 : 1, 0, Math.PI * 2); ctx.fill();
        ctx.fillStyle = '#ffe58f'; ctx.beginPath(); ctx.arc(ox + 45, oy + 22, ph === 1 ? 2 : 1, 0, Math.PI * 2); ctx.fill();
        const zOff = Math.floor(now / 180) % 6;
        ctx.fillStyle = accent; ctx.font = 'bold 11px sans-serif'; ctx.fillText('z', ox + 36, oy + 32 - zOff);
      } else if (ic === 4) {
        ctx.fillStyle = accent; ctx.beginPath(); ctx.roundRect(ox + 15, oy + 20, 24, 16, 4); ctx.fill();
        ctx.strokeStyle = accent; ctx.lineWidth = 2; ctx.beginPath(); ctx.arc(ox + 40, oy + 27, 4, 0, Math.PI * 2); ctx.stroke();
        ctx.fillStyle = '#b896a4'; ctx.fillRect(ox + 12, oy + 37, 30, 1.5);
        ctx.fillStyle = '#fff';
        for (let s = 0; s < 3; s++) {
          const sx = ox + 20 + s * 6 + Math.sin(now * 0.006 + s * 1.5) * 2;
          const sy = oy + 15 - (Math.floor(now / 140 + s * 3) % 7);
          ctx.fillRect(sx, sy, 1.5, 4);
        }
      } else if (ic === 5) {
        ctx.strokeStyle = '#4a1830'; ctx.lineWidth = 1.5; ctx.beginPath(); ctx.arc(ox + 28, oy + 22, 16, 0, Math.PI * 2); ctx.stroke();
        ctx.strokeStyle = accent; ctx.lineWidth = 2;
        const pts = [[8,24],[18,24],[23,13],[28,33],[33,17],[38,24],[48,24]];
        ctx.beginPath(); ctx.moveTo(ox + pts[0][0], oy + pts[0][1]);
        for (let i = 1; i < pts.length; i++) ctx.lineTo(ox + pts[i][0], oy + pts[i][1]);
        ctx.stroke();
        const dx = 8 + (Math.floor(now / 45) % 41);
        ctx.fillStyle = '#fff'; ctx.beginPath(); ctx.arc(ox + dx, oy + 24, 2, 0, Math.PI * 2); ctx.fill();
      } else {
        const wave = (now % 1200) / 1200;
        ctx.strokeStyle = `rgba(85, 242, 155, ${1 - wave})`; ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.arc(ox + 28, oy + 22, 11 + wave * 9, 0, Math.PI * 2); ctx.stroke();
        ctx.fillStyle = '#ff4977';
        ctx.beginPath(); ctx.arc(ox + 23, oy + 19, 6, 0, Math.PI * 2); ctx.fill();
        ctx.beginPath(); ctx.arc(ox + 33, oy + 19, 6, 0, Math.PI * 2); ctx.fill();
        ctx.beginPath(); ctx.moveTo(ox + 17, oy + 21); ctx.lineTo(ox + 39, oy + 21); ctx.lineTo(ox + 28, oy + 33); ctx.fill();
      }
      ctx.restore();

      ctx.fillStyle = '#1f0b15'; ctx.strokeStyle = accent + 'aa'; ctx.lineWidth = 1.2;
      ctx.beginPath(); ctx.roundRect(26, 94, 188, 64, 14); ctx.fill(); ctx.stroke();

      const txt = st.text || 'Думаю о тебе ♥';
      const sc = txt.length <= 14 ? 15 : 12;
      const lines = wrapLines(txt, txt.length <= 14 ? 14 : 24, 3);
      const step = sc + 5;
      const startY = 94 + (64 - lines.length * step) / 2;
      lines.forEach((ln, i) => centerText(ln, startY + i * step, sc, '#ffffff'));

      let tmLabel = 'Статус любимого ♥';
      if (st.epoch && st.epoch > 1700000000) {
        const d = new Date(st.epoch * 1000);
        tmLabel = 'Обновлено в ' + d.toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
      }
      centerText(tmLabel, 168, 10.5, '#b896a4');
      drawPageDots(5, false);
    }

    function drawWeatherIconJS(nowMs, code) {
      const ox = 92, oy = 34;
      const hasSun = (code === 0 || code === 1 || code === 2);
      const hasCloud = (code >= 1);
      const hasRain = ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95);
      const hasSnow = ((code >= 71 && code <= 77) || code === 85 || code === 86);
      const hasStorm = (code >= 95);

      if (hasSun) {
        const sx = ox + (hasCloud ? 36 : 28);
        const sy = oy + (hasCloud ? 15 : 21);
        const sr = hasCloud ? 9 : 11;
        ctx.fillStyle = '#ffe58f';
        ctx.beginPath(); ctx.arc(sx, sy, sr, 0, Math.PI * 2); ctx.fill();
        const base = nowMs * 0.0012;
        ctx.strokeStyle = '#ffe58f'; ctx.lineWidth = 1.6;
        for (let i = 0; i < 8; i++) {
          const a = base + i * (Math.PI / 4);
          ctx.beginPath();
          ctx.moveTo(sx + Math.cos(a) * (sr + 3), sy + Math.sin(a) * (sr + 3));
          ctx.lineTo(sx + Math.cos(a) * (sr + 6), sy + Math.sin(a) * (sr + 6));
          ctx.stroke();
        }
      }
      if (hasCloud) {
        const cy = oy + ((hasRain || hasSnow || hasStorm) ? 18 : 23);
        ctx.fillStyle = hasStorm ? '#afa5c8' : '#ebe8f8';
        ctx.beginPath();
        ctx.arc(ox + 19, cy, 8, 0, Math.PI * 2);
        ctx.arc(ox + 29, cy - 3, 10, 0, Math.PI * 2);
        ctx.arc(ox + 38, cy + 1, 7, 0, Math.PI * 2);
        ctx.roundRect(ox + 12, cy, 32, 10, 5);
        ctx.fill();
      }
      if (hasRain) {
        ctx.fillStyle = '#60b9ff';
        const phase = Math.floor((nowMs / 75) % 12);
        for (let d = 0; d < 3; d++) {
          ctx.fillRect(ox + 18 + d * 10, oy + 29 + ((phase + d * 4) % 12), 2, 4);
        }
      }
      if (hasSnow) {
        ctx.fillStyle = '#ffffff';
        const phase = Math.floor((nowMs / 100) % 12);
        for (let d = 0; d < 3; d++) {
          ctx.beginPath();
          ctx.arc(ox + 18 + d * 10 + Math.sin(nowMs * 0.004 + d) * 2, oy + 29 + ((phase + d * 4) % 12), 1.5, 0, Math.PI * 2);
          ctx.fill();
        }
      }
    }

    function drawWeatherScreen(now) {
      const w = state.weather || { valid: false, city: 'Москва', temp: 0, feels: 0, wind: 0, code: 0, desc: '—' };
      drawRings();
      centerText(`ПОГОДА • ${w.city || 'Москва'}`, 20, 10.5, '#ffb6c1');
      drawWeatherIconJS(now, w.valid ? (w.code || 0) : 2);

      if (w.valid) {
        const signT = w.temp > 0 ? '+' : '';
        const signF = w.feels > 0 ? '+' : '';
        centerText(`${signT}${w.temp}°C`, 81, 27, '#ffffff');
        centerText(w.desc || 'Ясно', 112, 10.5, '#ffe58f');
        centerText(`Ощущается ${signF}${w.feels}° • ${w.wind} м/с`, 127, 10, '#b896a4');
      } else {
        centerText('—', 81, 27, '#ffffff');
        centerText('—', 116, 11, '#b896a4');
      }

      const [sr, sg, sb] = evalShimmerRGB(now * 0.00035);
      ctx.fillStyle = '#1f0b15';
      ctx.strokeStyle = `rgb(${sr},${sg},${sb})`;
      ctx.lineWidth = 1.6;
      ctx.beginPath(); ctx.roundRect(36, 144, 168, 30, 13); ctx.fill(); ctx.stroke();
      let moodTxt = 'ГУЛЯБЕЛЬНО ❤';
      if (w.valid) {
        const c = w.code || 0;
        if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82) || c >= 95) moodTxt = 'ПОД ЗОНТОМ ❤';
        else if ((c >= 71 && c <= 77) || c === 85 || c === 86 || (w.temp || 0) <= 0) moodTxt = 'ОБНИМАТЬСЯ ❤';
        else if (c === 3 || c === 45 || c === 48 || (w.wind || 0) >= 8) moodTxt = 'УЮТНО ДОМА ❤';
        else moodTxt = 'ИДЁМ ГУЛЯТЬ ❤';
      }
      centerText(moodTxt, 151, 14.5, '#55f29b');

      drawMiniHeartSmooth(120, 189, now, true);
      drawPageDots(4, false);
    }

    function centerText(text, y, sizePx, color, xOff = 0) {
      ctx.fillStyle = color;
      ctx.font = `700 ${sizePx}px "Courier New", monospace`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      ctx.fillText(text, 120 + xOff, y);
    }

    function drawTicker(diffSec, dtSec) {
      const totalH = Math.floor(diffSec / 3600);
      const totalM = Math.floor(diffSec / 60);
      const str =
        `ВСЕГО: ${groupSpaces(totalH)} ${pluralUpper(totalH, 'ЧАС', 'ЧАСА', 'ЧАСОВ')}  •  ` +
        `${groupSpaces(totalM)} ${pluralUpper(totalM, 'МИНУТА', 'МИНУТЫ', 'МИНУТ')}  •  ` +
        `${groupSpaces(diffSec)} ${pluralUpper(diffSec, 'СЕКУНДА', 'СЕКУНДЫ', 'СЕКУНД')}  •  `;

      tctx.clearRect(0, 0, 152, 16);
      tctx.font = '700 11px "Courier New", monospace';
      tctx.textBaseline = 'middle';
      const loopW = Math.max(1, tctx.measureText(str).width);

      tickerScrollPx = (tickerScrollPx + 45 * dtSec) % loopW;
      tctx.fillStyle = '#ffb6c1';
      tctx.fillText(str, -tickerScrollPx, 8);
      tctx.fillText(str, -tickerScrollPx + loopW, 8);

      tctx.globalCompositeOperation = 'destination-in';
      const grad = tctx.createLinearGradient(0, 0, 152, 0);
      grad.addColorStop(0, 'rgba(0,0,0,0)');
      grad.addColorStop(14 / 152, 'rgba(0,0,0,1)');
      grad.addColorStop(1 - 14 / 152, 'rgba(0,0,0,1)');
      grad.addColorStop(1, 'rgba(0,0,0,0)');
      tctx.fillStyle = grad;
      tctx.fillRect(0, 0, 152, 16);
      tctx.globalCompositeOperation = 'source-over';

      ctx.drawImage(tickerOff, 44, 166);
    }

    function wrapTextJS(txt, maxChars) {
      const lines = [];
      for (const rawLine of txt.split('\n')) {
        const words = rawLine.trim().split(/\s+/);
        let cur = '';
        for (const w of words) {
          if (!cur) cur = w;
          else if ((cur + ' ' + w).length <= maxChars) cur += ' ' + w;
          else { lines.push(cur); cur = w; }
        }
        if (cur) lines.push(cur);
      }
      return lines.length ? lines : [''];
    }

    function drawMessagesScreen(now, dtSec, isTextPage = false) {
      if (!isTextPage && hasDecodedCard) {
        ctx.drawImage(cardOff, 0, 0);
        drawRings();
        drawPageDots(2, true);
        return;
      }

      drawRings();
      drawMiniHeartSmooth(120, 27, now, true);
      const isUnread = isTextPage ? state.plain_unread : state.unread;
      const hdrText = isTextPage
        ? (isUnread ? 'НОВОЕ СООБЩЕНИЕ' : 'СООБЩЕНИЕ')
        : (isUnread ? 'НОВОЕ ПОСЛАНИЕ' : 'ПОСЛАНИЕ ДЛЯ ТЕБЯ');
      centerText(hdrText, 45, 11, isUnread ? '#ffe58f' : '#ffb6c1');

      const latest = isTextPage
        ? { text: state.plain_text || 'Нет сообщений', time: state.plain_time || 0 }
        : ((state.messages && state.messages[0]) ? state.messages[0] : { text: 'Я всегда рядом ❤️', time: 0 });
      const cacheKey = (isTextPage ? 'T:' : 'M:') + latest.text;
      if (cacheKey !== lastMsgRenderedText) {
        lastMsgRenderedText = cacheKey;
        msgScrollY = 0; msgScrollDir = 1; msgPauseSec = 1.8;
      }

      let bigFont = latest.text.length <= 40;
      let lines = wrapTextJS(latest.text, bigFont ? 15 : 24);
      if (bigFont && lines.length > 3) {
        bigFont = false;
        lines = wrapTextJS(latest.text, 24);
      }

      const lineStep = bigFont ? 21 : 14;
      const totalH = lines.length * lineStep;
      const visibleH = 70;

      mctx.clearRect(0, 0, 172, 86);
      mctx.fillStyle = '#1f0b15';
      mctx.beginPath();
      mctx.roundRect(0, 0, 172, 86, 14);
      mctx.fill();

      let startY = 8;
      if (totalH <= visibleH) {
        startY = (86 - totalH) / 2 + 2;
      } else {
        const maxS = totalH - visibleH;
        if (msgPauseSec > 0) msgPauseSec -= dtSec;
        else {
          msgScrollY += msgScrollDir * 18 * dtSec;
          if (msgScrollY >= maxS) { msgScrollY = maxS; msgScrollDir = -1; msgPauseSec = 2.0; }
          else if (msgScrollY <= 0) { msgScrollY = 0; msgScrollDir = 1; msgPauseSec = 2.0; }
        }
        startY = 8 - msgScrollY;
      }

      mctx.save();
      mctx.beginPath();
      mctx.roundRect(3, 4, 166, 78, 11);
      mctx.clip();
      mctx.fillStyle = '#ffffff';
      mctx.font = `700 ${bigFont ? 15 : 11.5}px "Courier New", monospace`;
      mctx.textAlign = 'center';
      mctx.textBaseline = 'top';
      lines.forEach((ln, i) => mctx.fillText(ln, 86, startY + i * lineStep));
      mctx.restore();

      mctx.strokeStyle = isUnread ? '#ff4977' : '#4a1830';
      mctx.lineWidth = 1.5;
      mctx.beginPath();
      mctx.roundRect(0.75, 0.75, 170.5, 84.5, 14);
      mctx.stroke();

      ctx.drawImage(msgOff, 34, 62);
      const d = latest.time ? new Date(latest.time * 1000) : new Date();
      const tStr = latest.time ? `Получено ${String(d.getDate()).padStart(2,'0')}.${String(d.getMonth()+1).padStart(2,'0')} в ${String(d.getHours()).padStart(2,'0')}:${String(d.getMinutes()).padStart(2,'0')}` : '—';
      centerText(tStr, 156, 10, '#b896a4');
      centerText('Кн.1 (10с) - послать ♥', 173, 10, '#ffb6c1');
      drawPageDots(isTextPage ? 3 : 2, false);
    }

    function beginTransition(target) {
      pctx.clearRect(0, 0, 240, 240);
      pctx.drawImage(canvas, 0, 0);
      transActive = true;
      transPhase = 0;
      transTarget = target;
      transStartMs = performance.now();
    }

    function renderLoop(now) {
      const dtSec = Math.min(0.08, Math.max(0.001, (now - lastRenderTs) / 1000));
      lastRenderTs = now;

      if (transActive && !otaSimActive) {
        const dt = now - transStartMs;
        if (transPhase === 0) {
          const p = Math.min(1, dt / 150);
          const ease = p * p * (3 - 2 * p);
          const curR = Math.max(0, 118 * (1 - ease));

          ctx.fillStyle = '#000000';
          ctx.fillRect(0, 0, 240, 240);
          if (curR > 1) {
            ctx.save();
            ctx.beginPath();
            ctx.arc(120, 120, curR, 0, Math.PI * 2);
            ctx.clip();
            ctx.drawImage(prevFrameOff, 0, 0);
            ctx.restore();

            const [sr, sg, sb] = evalShimmerRGB(now * 0.0012);
            ctx.strokeStyle = `rgb(${sr},${sg},${sb})`;
            ctx.lineWidth = 3;
            ctx.beginPath();
            ctx.arc(120, 120, curR, 0, Math.PI * 2);
            ctx.stroke();
          }

          if (p >= 1) {
            screenMode = transTarget;
            transPhase = 1;
            transStartMs = now;
          }
          requestAnimationFrame(renderLoop);
          return;
        }

        // Фаза 1: раскрытие новой страницы
        const p = Math.min(1, dt / 230);
        const easeOut = 1 - Math.pow(1 - p, 3);
        ctx.fillStyle = '#000000';
        ctx.fillRect(0, 0, 240, 240);

        if (screenMode === 'messages' && hasDecodedCard) {
          const halfH = Math.max(4, 120 * easeOut);
          ctx.save();
          ctx.beginPath();
          ctx.rect(0, 120 - halfH, 240, halfH * 2);
          ctx.clip();
          ctx.drawImage(cardOff, 0, 0);
          drawRings();
          ctx.restore();

          if (p < 1) {
            const [sr, sg, sb] = evalShimmerRGB(now * 0.001);
            ctx.fillStyle = `rgb(${sr},${sg},${sb})`;
            ctx.fillRect(24, 120 - halfH - 1, 192, 2);
            ctx.fillRect(24, 120 + halfH - 1, 192, 2);
          } else {
            drawPageDots(2, true);
          }
        } else if (screenMode === 'timer') {
          const slide = Math.round(62 * (1 - easeOut));
          drawRings();
          drawMiniHeartSmooth(120, 31, now, false);
          centerText('МЫ ВМЕСТЕ', 47, 15.5, '#ffb6c1');

          const nowMsReal = Date.now() + extraSecondsTest * 1000;
          const nowUtc = Math.floor(nowMsReal / 1000);
          const startUtc = 1792540800 - state.tz_offset * 3600;
          const diff = Math.max(0, nowUtc - startUtc);
          const days = Math.floor(diff / 86400);
          const hrs = String(Math.floor((diff % 86400) / 3600)).padStart(2, '0');
          const mins = String(Math.floor((diff % 3600) / 60)).padStart(2, '0');
          const secs = String(diff % 60).padStart(2, '0');
          const daysStr = days < 10000 ? String(days) : groupSpaces(days);

          ctx.save();
          ctx.beginPath(); ctx.rect(36, 65, 168, 60); ctx.clip();
          centerText(daysStr, 68, daysStr.length <= 7 ? 32 : 24, '#ffffff', slide);
          centerText(pluralUpper(days, 'ДЕНЬ', 'ДНЯ', 'ДНЕЙ'), 106, 15.5, '#ff4977', slide);
          ctx.restore();

          ctx.save();
          ctx.beginPath(); ctx.rect(36, 128, 168, 32); ctx.clip();
          ctx.fillStyle = '#1f0b15'; ctx.strokeStyle = '#4a1830';
          ctx.beginPath(); ctx.roundRect(36 - slide, 129, 168, 30, 12); ctx.fill(); ctx.stroke();
          centerText(`${hrs} : ${mins} : ${secs}`, 136, 16, '#ffffff', -slide);
          ctx.restore();

          drawPageDots(1, false);
        } else if (screenMode === 'heart_ip') {
          const elastic = p < 0.72
            ? 0.22 + 0.91 * Math.sin((p / 0.72) * (Math.PI * 0.5))
            : 1.13 - 0.13 * ((p - 0.72) / 0.28);
          drawRings();
          centerText('СТАНЦИЯ ОТНОШЕНИЙ', 19, 10.5, '#ffb6c1');
          draw3DHeart(120, 86, getBeatScale(now) * elastic, false, now);
          centerText('Адрес страницы:', 143, 10.5, '#b896a4');
          centerText(state.ip, 157, 17, '#ffe58f');
          centerText(`Wi-Fi ок • #${state.commit || '95980b7'}`, 180, 10.5, '#55f29b');
          drawPageDots(6, false);
        } else if (screenMode === 'my_status') {
          drawMyStatusScreen(now);
        } else if (screenMode === 'weather') {
          drawWeatherScreen(now);
        } else {
          drawMessagesScreen(now, dtSec, screenMode === 'text_msg');
        }

        if (p >= 1) transActive = false;
        requestAnimationFrame(renderLoop);
        return;
      }

      ctx.fillStyle = '#000000';
      ctx.fillRect(0, 0, 240, 240);

      if (b1DownAt > 0 && !b1HoldCompleted && !otaSimActive && now >= bannerUntil) {
        const heldMs = now - b1DownAt;
        if (heldMs >= BTN1_HOLD_UI_DELAY_MS) {
          if (heldMs >= BTN1_HEART_HOLD_MS) {
            b1HoldCompleted = true;
            b1DownAt = 0;
            pressBtn1(true);
          } else {
            const p = Math.min(1, heldMs / BTN1_HEART_HOLD_MS);
            const secLeft = Math.ceil((BTN1_HEART_HOLD_MS - heldMs) / 1000);
            ctx.fillStyle = '#080507';
            ctx.fillRect(0, 0, 240, 240);
            drawRings();
            centerText('ОТПРАВИТЬ СЕРДЕЧКО?', 22, 10.5, '#ffb6c1');
            draw3DHeart(120, 82, getBeatScale(now) * (0.78 + 0.28 * p), p >= 0.2, now);
            centerText(`Удерживай: ${secLeft} с`, 134, 11.5, '#ffe58f');

            const bx = 38, by = 154, bw = 164, bh = 22;
            ctx.fillStyle = '#1f0b15';
            ctx.beginPath(); ctx.roundRect(bx, by, bw, bh, 11); ctx.fill();
            const fillW = Math.round((bw - 6) * p);
            if (fillW > 0) {
              const g = ctx.createLinearGradient(bx + 3, 0, bx + 3 + fillW, 0);
              const [r1, g1, b1] = evalShimmerRGB(now * 0.0008);
              const [r2, g2, b2] = evalShimmerRGB(now * 0.0008 + 0.45);
              g.addColorStop(0, `rgb(${r1},${g1},${b1})`);
              g.addColorStop(1, `rgb(${r2},${g2},${b2})`);
              ctx.fillStyle = g;
              ctx.beginPath(); ctx.roundRect(bx + 3, by + 3, fillW, bh - 6, 8); ctx.fill();
            }
            const [sr, sg, sb] = evalShimmerRGB(now * 0.0012);
            ctx.strokeStyle = `rgb(${sr},${sg},${sb})`;
            ctx.lineWidth = 1.5;
            ctx.beginPath(); ctx.roundRect(bx, by, bw, bh, 11); ctx.stroke();

            centerText('Отпусти — отмена', 194, 10.5, '#b896a4');
            requestAnimationFrame(renderLoop);
            return;
          }
        }
      }

      if (otaSimActive) {
        const elapsed = now - otaSimStart;
        const pct = Math.min(100, Math.floor(elapsed / 32));
        drawRings();
        centerText('ОБНОВЛЕНИЕ СИСТЕМЫ', 20, 10.5, '#ffb6c1');
        draw3DHeart(120, 82, getBeatScale(now), true, now);
        centerText(`Коммит #${otaTargetVer}`, 134, 11, '#ffe58f');

        ctx.fillStyle = '#1f0b15'; ctx.strokeStyle = '#4a1830';
        ctx.beginPath(); ctx.roundRect(36, 150, 168, 30, 12); ctx.fill(); ctx.stroke();
        const fillW = Math.round((160 * pct) / 100);
        if (fillW > 0) {
          ctx.fillStyle = '#ff4977';
          ctx.beginPath(); ctx.roundRect(40, 154, fillW, 22, 9); ctx.fill();
        }
        centerText(`${pct}%`, 158, 16, '#ffffff');
        if (pct >= 100) {
          centerText('ГОТОВО! ПЕРЕЗАПУСК...', 188, 10.5, '#55f29b');
          if (elapsed > 4300) {
            otaSimActive = false;
            fetch('/api/sim_bump_ver?commit=' + encodeURIComponent(otaTargetVer), { method: 'POST' });
          }
        }
      } else if (now < bannerUntil) {
        drawRings();
        draw3DHeart(120, 80, getBeatScale(now), false, now);
        centerText('ПАРОЛЬ СОХРАНЁН!', 140, 11, '#55f29b');
        centerText(state.ip, 158, 17, '#ffe58f');
        centerText('Открой в браузере', 184, 10.5, '#b896a4');
      } else if (now < pulseUntil) {
        const pElapsed = (now - (pulseUntil - 2400)) / 240;
        const elastic = pElapsed < 1
          ? (0.25 + 0.88 * Math.sin(pElapsed * Math.PI * 0.5))
          : 1.0;
        drawRings();
        centerText('СТАНЦИЯ ОТНОШЕНИЙ', 19, 10.5, '#ffb6c1');
        draw3DHeart(120, 84, getBeatScale(now) * elastic, true, now);
        centerText(phrases[phraseIdx], 150, 16.5, '#ffffff');
        if (pulseReplyShown) {
          centerText('Ответное ♥ отправлено!', 180, 10.5, '#55f29b');
        }
      } else if (screenMode === 'ap') {
        drawRings();
        centerText('СТАНЦИЯ ОТНОШЕНИЙ', 19, 10.5, '#ffb6c1');
        draw3DHeart(120, 88, getBeatScale(now), false, now);
        centerText('Подключи Wi-Fi:', 145, 10.5, '#b896a4');
        centerText('love', 160, 19, '#55f29b');
        centerText('Сайт: 192.168.4.1', 185, 11, '#ffe58f');
      } else if (screenMode === 'messages') {
        drawMessagesScreen(now, dtSec, false);
      } else if (screenMode === 'text_msg') {
        drawMessagesScreen(now, dtSec, true);
      } else if (screenMode === 'weather') {
        drawWeatherScreen(now);
      } else if (screenMode === 'my_status') {
        drawMyStatusScreen(now);
      } else if (screenMode === 'heart_ip') {
        drawRings();
        centerText('СТАНЦИЯ ОТНОШЕНИЙ', 19, 10.5, '#ffb6c1');
        draw3DHeart(120, 86, getBeatScale(now), false, now);
        centerText('Адрес страницы:', 143, 10.5, '#b896a4');
        centerText(state.ip, 157, 17, '#ffe58f');
        centerText(state.commit ? `Wi-Fi ок • #${state.commit}` : `Wi-Fi ок • v${state.version || '1.3.8'}`, 180, 10.5, '#55f29b');
        drawPageDots(6, false);
      } else {
        drawRings();
        drawMiniHeartSmooth(120, 31, now, false);
        centerText('МЫ ВМЕСТЕ', 47, 15.5, '#ffb6c1');

        const nowMsReal = Date.now() + extraSecondsTest * 1000;
        const nowUtc = Math.floor(nowMsReal / 1000);
        const fracSec = (nowMsReal % 1000) / 1000;
        const startUtc = 1792540800 - state.tz_offset * 3600;
        const diff = Math.max(0, nowUtc - startUtc);
        const days = Math.floor(diff / 86400);
        const hrs = String(Math.floor((diff % 86400) / 3600)).padStart(2, '0');
        const mins = String(Math.floor((diff % 3600) / 60)).padStart(2, '0');
        const secs = diff % 60;
        const secsStr = String(secs).padStart(2, '0');

        const daysStr = days < 10000 ? String(days) : groupSpaces(days);
        const daysFontPx = daysStr.length <= 7 ? 32 : 24;
        centerText(daysStr, 68, daysFontPx, '#ffffff');
        centerText(pluralUpper(days, 'ДЕНЬ', 'ДНЯ', 'ДНЕЙ'), 106, 15.5, '#ff4977');

        ctx.fillStyle = '#1f0b15'; ctx.strokeStyle = '#4a1830';
        ctx.beginPath(); ctx.roundRect(36, 129, 168, 30, 12); ctx.fill(); ctx.stroke();
        centerText(`${hrs} : ${mins} : ${secsStr}`, 136, 16, '#ffffff');

        drawTicker(diff, dtSec);
        drawPageDots(1, false);
        drawOrbitShimmerHeart(secs + fracSec, now);
      }

      if (b2DownAt > 0) {
        const held = now - b2DownAt;
        if (held >= 1200 && held < 4000) {
          const left = 4 - Math.floor(held / 1000);
          centerText(`Сброс Wi-Fi: ${left}...`, 190, 10.5, '#ffe58f');
        } else if (held >= 4000) {
          b2DownAt = 0;
          forceApMode();
        }
      }

      requestAnimationFrame(renderLoop);
    }

    function btn1Down(e) {
      if (e && e.cancelable) e.preventDefault();
      if (otaSimActive || performance.now() < bannerUntil) return;
      b1DownAt = performance.now();
      b1HoldCompleted = false;
    }

    function btn1Up(e) {
      if (e && e.cancelable) e.preventDefault();
      b1DownAt = 0;
      b1HoldCompleted = false;
    }

    async function pressBtn1(fromHw) {
      if (otaSimActive) return;
      if (performance.now() < pulseUntil) {
        phraseIdx = (phraseIdx + 1) % phrases.length;
      }
      pulseReplyShown = state.connected;
      pulseUntil = performance.now() + 2400;
      if (fromHw) {
        fetch('/api/pulse?from_hw=1', { method: 'POST' }).catch(() => {});
      }
    }

    let lastSimPageSwitchMs = 0;
    function btn2Down(e) {
      if (e && e.cancelable) e.preventDefault();
      if (otaSimActive) return;
      const now = performance.now();
      b2DownAt = now;
      if (state.connected && (now - lastSimPageSwitchMs) >= 320) {
        lastSimPageSwitchMs = now;
        const cur = transActive ? transTarget : screenMode;
        if (now < pulseUntil) {
          pulseUntil = 0;
          beginTransition('timer');
        } else if (cur === 'timer') {
          fetch('/api/mark_read', { method: 'POST' }).catch(() => {});
          beginTransition('messages');
        } else if (cur === 'messages') {
          fetch('/api/mark_read', { method: 'POST' }).catch(() => {});
          beginTransition('text_msg');
        } else if (cur === 'text_msg') {
          beginTransition('weather');
        } else if (cur === 'weather') {
          beginTransition('my_status');
        } else if (cur === 'my_status') {
          beginTransition('heart_ip');
        } else {
          beginTransition('timer');
        }
      }
    }
    function btn2Up(e) {
      if (e && e.cancelable) e.preventDefault();
      b2DownAt = 0;
    }

    function toggleBigDaysTest() {
      const btn = document.getElementById('bigDaysBtn');
      if (extraSecondsTest === 0) {
        extraSecondsTest = 3650 * 86400;
        btn.classList.add('active');
        if (!state.connected) forceConnectedMode();
      } else {
        extraSecondsTest = 0;
        btn.classList.remove('active');
      }
    }

    function startSimOta() {
      if (otaSimActive) return;
      otaTargetVer = state.ota_target_commit || 'a4f9c21';
      otaSimActive = true;
      otaSimStart = performance.now();
    }

    async function pollBackend() {
      try {
        const res = await fetch('/api/sim_state');
        const d = await res.json();
        const wasConn = state.connected;
        state = d;
        canvas.style.transition = 'transform 0.28s cubic-bezier(0.22, 1, 0.36, 1)';
        canvas.style.transform = `rotate(${((d.rotation || 0) & 3) * 90}deg)`;
        if (!wasConn && d.connected) {
          bannerUntil = performance.now() + 2800;
          screenMode = 'timer';
        } else if (wasConn && !d.connected) {
          screenMode = 'ap';
        }
        if (d.pulse_seq !== lastPulseSeq) {
          lastPulseSeq = d.pulse_seq;
          pressBtn1(false);
        }
        if (d.ota_seq !== lastOtaSeq) {
          lastOtaSeq = d.ota_seq;
          startSimOta();
        }
        if (d.msg_seq !== lastMsgSeq) {
          lastMsgSeq = d.msg_seq;
          await fetchLatestCardIfAny();
          if (d.connected) beginTransition('messages');
        }
        if (d.plain_seq !== lastPlainSeq) {
          lastPlainSeq = d.plain_seq;
          if (d.connected) beginTransition('text_msg');
        }
        if (d.status_seq !== lastStatusSeq) {
          lastStatusSeq = d.status_seq;
          if (d.connected) beginTransition('my_status');
        }
      } catch (e) {}
    }

    async function forceApMode() {
      await fetch('/api/sim_mode?connected=0', { method: 'POST' });
      await pollBackend();
      screenMode = 'ap';
      switchRightView('site');
    }

    async function forceConnectedMode() {
      await fetch('/api/sim_mode?connected=1', { method: 'POST' });
      await pollBackend();
      beginTransition('timer');
      bannerUntil = 0;
      document.getElementById('siteFrame').contentWindow.location.reload();
    }

    setInterval(pollBackend, 600);
    pollBackend();
    requestAnimationFrame(renderLoop);
  </script>
</body>
</html>
"""


def build_admin_payload():
    latest_card = STATE["messages"][0]["text"] if STATE["messages"] else ""
    return {
        "ver": STATE["version"],
        "commit": STATE.get("commit", ""),
        "ssid": STATE["ssid"] if STATE["connected"] else "",
        "saved_ssid": STATE["saved_ssid"],
        "pass": STATE.get("saved_pass", ""),
        "ip": STATE["ip"],
        "city": STATE["weather"]["city"],
        "plain": STATE.get("plain_text", ""),
        "card": latest_card,
        "st_icon": STATE["my_status"]["icon"],
        "st_text": STATE["my_status"]["text"],
        "rssi": -48 if STATE["connected"] else 0,
        "heap": 148200,
        "up": int(time.time() - START_BOOT_TS),
        "tz": STATE["tz_offset"],
        "rot": STATE.get("rotation", 0),
        "w_ok": STATE["weather"]["valid"],
        "w_temp": STATE["weather"]["temp"],
        "ota": 2,
        "nets": [{"s": n["ssid"], "r": n["rssi"], "o": n["open"]} for n in MOCK_NETWORKS],
    }


class PreviewHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(fmt % args)

    def _send_json(self, obj, code=200):
        raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _send_html(self, html_str, code=200):
        raw = html_str.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if path == "/":
            self._send_html(STUDIO_HTML)
        elif path == "/site":
            self._send_html(load_esp_html())
        elif path == "/sender":
            self._send_html(load_sender_html())
        elif path == "/api/status":
            self._send_json({
                "connected": STATE["connected"],
                "ssid": STATE["ssid"],
                "saved_ssid": STATE["saved_ssid"],
                "has_saved_pass": STATE["has_saved_pass"],
                "ip": STATE["ip"],
                "tz_offset": STATE["tz_offset"],
                "rotation": STATE.get("rotation", 0),
                "weather_city": STATE["weather"]["city"],
                "version": STATE["version"],
                "commit": STATE.get("commit", ""),
            })
        elif path == "/api/weather":
            w = dict(STATE["weather"])
            if w.get("valid"):
                c = int(w.get("code", 0))
                if (51 <= c <= 67) or (80 <= c <= 82) or c >= 95:
                    w["mood"] = "ПОД ЗОНТОМ ❤"
                elif (71 <= c <= 77) or c in (85, 86) or int(w.get("temp", 0)) <= 0:
                    w["mood"] = "ОБНИМАТЬСЯ ❤"
                elif c in (3, 45, 48) or int(w.get("wind", 0)) >= 8:
                    w["mood"] = "УЮТНО ДОМА ❤"
                else:
                    w["mood"] = "ИДЁМ ГУЛЯТЬ ❤"
            else:
                w["mood"] = "ГУЛЯБЕЛЬНО ❤"
            self._send_json(w)
        elif path == "/api/ota_status":
            c_ref = STATE.get("ota_target_commit") or STATE.get("commit", "")
            self._send_json({"code": 4, "ver": c_ref, "commit": c_ref})
        elif path == "/api/my_status":
            self._send_json(STATE["my_status"])
        elif path == "/api/messages":
            latest = STATE["messages"][0] if STATE["messages"] else {"text": "", "time": 0, "has_card": False}
            self._send_json({
                "unread": STATE["unread"],
                "latest_text": latest["text"],
                "latest_time": latest["time"],
                "latest_has_card": latest.get("has_card", False),
                "plain_text": STATE.get("plain_text", "—"),
                "plain_time": STATE.get("plain_time", 0),
                "plain_unread": STATE.get("plain_unread", False),
                "history": STATE["messages"][:5],
            })
        elif path == "/api/card":
            idx = int((qs.get("idx") or ["0"])[0])
            if 0 <= idx < 5 and CARDS[idx] is not None:
                data = CARDS[idx]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_response(404)
                self.end_headers()
        elif path == "/api/sim_state":
            self._send_json(STATE)
        elif path == "/api/sim_admin":
            self._send_json(build_admin_payload())
        elif path == "/api/scan":
            self._send_json(MOCK_NETWORKS)
        else:
            self._send_html(STUDIO_HTML)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        length = int(self.headers.get("Content-Length", "0"))
        body_bytes = self.rfile.read(length) if length > 0 else b""

        if path == "/api/sync_time":
            self._send_json({"ok": True})
        elif path == "/api/mark_read":
            STATE["unread"] = False
            STATE["plain_unread"] = False
            self._send_json({"ok": True})
        elif path == "/api/pulse":
            from_hw = (qs.get("from_hw") or ["0"])[0] == "1"
            if not from_hw:
                STATE["pulse_seq"] += 1
            STATE["unread"] = False
            t_str = time.strftime("%H:%M:%S")
            STATE["replies"].append({
                "text": "Она нажала кнопку на станции и прислала тебе сердечко!",
                "time": t_str,
            })
            STATE["replies"] = STATE["replies"][-5:]
            self._send_json({"ok": True})
        elif path == "/api/sim_push_card":
            if len(body_bytes) >= 70 and body_bytes[:4] == b"LVC1":
                cap_len = struct.unpack_from("<H", body_bytes, 8)[0]
                cap = body_bytes[70:70 + cap_len].decode("utf-8", errors="replace") if cap_len else "Открытка для тебя ❤️"
                CARDS.insert(0, body_bytes)
                del CARDS[5:]
                STATE["messages"].insert(0, {"text": cap, "time": int(time.time()), "has_card": True})
                STATE["messages"] = STATE["messages"][:5]
                STATE["unread"] = True
                STATE["msg_seq"] += 1
                self._send_json({"ok": True, "size": len(body_bytes)})
            else:
                self._send_json({"ok": False}, 400)
        elif path == "/api/sim_push_msg":
            try:
                payload = json.loads(body_bytes.decode("utf-8"))
                txt = (payload.get("message") or "").strip()
                if txt:
                    STATE["plain_text"] = txt
                    STATE["plain_time"] = int(time.time())
                    STATE["plain_unread"] = True
                    STATE["plain_seq"] += 1
            except Exception:
                pass
            self._send_json({"ok": True})
        elif path == "/api/text_msg":
            form = urllib.parse.parse_qs(body_bytes.decode("utf-8"))
            txt = (form.get("text") or [""])[0].strip()
            if txt:
                STATE["plain_text"] = txt
                STATE["plain_time"] = int(time.time())
                STATE["plain_unread"] = True
                STATE["plain_seq"] += 1
                self._send_json({"ok": True})
            else:
                self._send_json({"ok": False}, 400)
        elif path == "/api/ota_check":
            STATE["ota_target_commit"] = format(int(time.time()) & 0xFFFFFFF, "07x")
            STATE["ota_seq"] += 1
            self._send_json({
                "ok": True,
                "message": "В main найден новый коммит! Смотри на круглый экран — началась установка."
            })
        elif path == "/api/sim_bump_ver":
            ver = (qs.get("ver") or [None])[0]
            cmt = (qs.get("commit") or [None])[0]
            if ver:
                STATE["version"] = ver
            if cmt:
                STATE["commit"] = cmt
            self._send_json({"ok": True})
        elif path == "/api/weather_city":
            form = urllib.parse.parse_qs(body_bytes.decode("utf-8"))
            city = (form.get("city") or ["Красноярск"])[0].strip() or "Красноярск"
            STATE["weather"]["city"] = city
            STATE["weather"]["valid"] = True
            self._send_json({"ok": True})
        elif path == "/api/rotation":
            r_raw = (qs.get("rot") or [None])[0]
            if r_raw is None:
                form = urllib.parse.parse_qs(body_bytes.decode("utf-8"))
                r_raw = (form.get("rot") or ["0"])[0]
            STATE["rotation"] = int(r_raw) & 3
            self._send_json({"ok": True, "rotation": STATE["rotation"]})
        elif path == "/api/sim_admin_cmd":
            try:
                payload = json.loads(body_bytes.decode("utf-8"))
                cmd = (payload.get("cmd") or "").strip()
                if cmd == "OTA":
                    STATE["ota_seq"] += 1
                elif cmd.startswith("ROT|"):
                    STATE["rotation"] = int(cmd.split("|")[1]) & 3
                elif cmd.startswith("STATUS_SET|"):
                    parts = cmd.split("|", 2)
                    if len(parts) >= 3:
                        STATE["my_status"]["icon"] = int(parts[1]) % 7
                        STATE["my_status"]["text"] = parts[2].strip() or "Думаю о тебе ♥"
                        STATE["my_status"]["epoch"] = int(time.time())
                        STATE["status_seq"] += 1
                elif cmd.startswith("CITY|"):
                    parts = cmd.split("|")
                    if len(parts) >= 5:
                        STATE["weather"]["city"] = parts[1]
                        STATE["tz_offset"] = int(parts[4])
            except Exception:
                pass
            self._send_json({"ok": True, "state": build_admin_payload()})
        elif path == "/api/connect":
            form = urllib.parse.parse_qs(body_bytes.decode("utf-8"))
            ssid = (form.get("ssid") or [""])[0].strip()
            pw = (form.get("pass") or [""])[0]
            tz = int((form.get("tz") or ["7"])[0])
            STATE["tz_offset"] = tz
            if ssid:
                STATE["connected"] = True
                STATE["ssid"] = ssid
                STATE["saved_ssid"] = ssid
                if pw:
                    STATE["saved_pass"] = pw
                STATE["has_saved_pass"] = True
                STATE["ip"] = "192.168.1.48"
                self._send_json({"ok": True, "ip": STATE["ip"]})
            else:
                self._send_json({"ok": False})
        elif path == "/api/sim_mode":
            conn = (qs.get("connected") or ["0"])[0] == "1"
            if conn:
                STATE["connected"] = True
                STATE["ssid"] = STATE["saved_ssid"] or "Home_WiFi_5G"
                STATE["ip"] = "192.168.1.48"
            else:
                STATE["connected"] = False
                STATE["ssid"] = ""
                STATE["ip"] = "192.168.4.1"
            self._send_json({"ok": True})
        else:
            self._send_json({"ok": True})


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", 8000), PreviewHandler)
    print("Preview studio listening on http://0.0.0.0:8000", flush=True)
    server.serve_forever()
