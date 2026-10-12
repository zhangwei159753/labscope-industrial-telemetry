<script setup lang="ts">
import { inject, onBeforeUnmount, onMounted, ref } from 'vue'
import { RouterLink } from 'vue-router'
import { api, type Device, type Stats } from '../api/client'
import DeviceStatusBadge from '../components/DeviceStatusBadge.vue'

const siteId = inject('siteId', ref('plant-a'))
const stats = ref<Stats | null>(null)
const devices = ref<Device[]>([])
const error = ref('')
const loading = ref(true)
const updatedAt = ref('—')
let refreshTimer = 0

async function refresh() {
  try {
    const [nextStats, nextDevices] = await Promise.all([
      api.stats(siteId.value), api.devices({ siteId: siteId.value, limit: 6 }),
    ])
    stats.value = nextStats
    devices.value = nextDevices.devices
    error.value = ''
    updatedAt.value = new Intl.DateTimeFormat('zh-CN', { hour: '2-digit', minute: '2-digit', second: '2-digit' }).format(new Date())
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : '接口暂时不可用'
  } finally { loading.value = false }
}

onMounted(() => { void refresh(); refreshTimer = window.setInterval(() => void refresh(), 5000) })
onBeforeUnmount(() => window.clearInterval(refreshTimer))
</script>

<template>
  <section class="overview-hero panel">
    <div class="hero-caption"><span class="signal-bars"><i></i><i></i><i></i></span> LIVE TELEMETRY / 设备运行态势</div>
    <div class="hero-main"><div><h2>看见每一次<br><em>设备脉搏。</em></h2><p>遥测接入状态与近期样本，在一个视图内持续更新。</p></div>
      <div class="hero-readout"><span>近 60 秒采样</span><strong>{{ stats?.samples_last_minute?.toLocaleString() ?? '—' }}</strong><small>RECEIVED / MIN</small><div class="readout-line"></div></div>
    </div>
    <div class="hero-grid-mark" aria-hidden="true"></div>
  </section>

  <div v-if="error" class="error-banner" role="alert"><span>!</span>{{ error }}<button @click="refresh">重试</button></div>

  <section class="metric-grid" aria-label="设备运行指标">
    <article class="metric-card"><div class="metric-head"><span>接入设备</span><span class="metric-index">FLEET / 01</span></div><strong>{{ loading ? '—' : stats?.devices ?? 0 }}</strong><small>注册于当前遥测平台</small><span class="metric-watermark">▦</span></article>
    <article class="metric-card metric-green"><div class="metric-head"><span>在线设备</span><span class="metric-index">HEALTHY / 02</span></div><strong>{{ loading ? '—' : stats?.online_devices ?? 0 }}</strong><small><i class="inline-dot"></i> 最近 60 秒内有有效上报</small><span class="metric-watermark">◉</span></article>
    <article class="metric-card metric-amber"><div class="metric-head"><span>数据过期</span><span class="metric-index">ATTENTION / 03</span></div><strong>{{ loading ? '—' : stats?.stale_devices ?? 0 }}</strong><small>超过 60 秒未收到遥测</small><span class="metric-watermark">⌁</span></article>
    <article class="metric-card"><div class="metric-head"><span>采样吞吐</span><span class="metric-index">INGEST / 04</span></div><strong>{{ stats?.ingest_messages_per_second?.toFixed(1) ?? '0.0' }}<b>Hz</b></strong><small>基于最近 60 秒统计</small><span class="metric-watermark">↗</span></article>
  </section>

  <section class="panel fleet-panel">
    <div class="section-heading"><div><p class="section-kicker">DEVICE FLEET</p><h2>设备运行列表</h2></div><div class="section-tools"><span class="refresh-stamp">更新于 {{ updatedAt }}</span><RouterLink class="text-link" to="/devices">全部设备 <span>↗</span></RouterLink></div></div>
    <div class="table-scroll"><table class="device-table"><thead><tr><th>设备 / DEVICE</th><th>型号</th><th>状态</th><th>最后上报</th><th>序号</th><th>缺口</th><th></th></tr></thead>
      <tbody v-if="devices.length"><tr v-for="device in devices" :key="`${device.site_id}/${device.device_id}`">
        <td><RouterLink class="device-name" :to="`/devices/${encodeURIComponent(device.device_id)}?site_id=${encodeURIComponent(siteId)}`"><span class="device-glyph">◈</span><span><strong>{{ device.display_name || device.device_id }}</strong><small>{{ device.device_id }}</small></span></RouterLink></td>
        <td class="muted-cell">{{ device.model || '通用遥测' }}</td><td><DeviceStatusBadge :status="device.status" /></td>
        <td class="mono-cell">{{ device.last_seen_at ? new Date(device.last_seen_at).toLocaleTimeString('zh-CN', { hour12: false }) : '—' }}</td>
        <td class="mono-cell">{{ device.last_seq }}</td><td><span :class="device.sequence_gap_count ? 'gap-count' : 'muted-cell'">{{ device.sequence_gap_count }}</span></td>
        <td class="row-arrow">↗</td>
      </tr></tbody>
      <tbody v-else><tr><td colspan="7"><div class="empty-row"><span>◌</span><strong>{{ loading ? '正在读取设备台账' : '当前站点还没有设备' }}</strong><small>{{ loading ? '正在连接遥测 API' : '请先通过设备发放工具注册一台设备' }}</small></div></td></tr></tbody>
    </table></div>
    <div class="table-foot"><span><i class="foot-dot"></i>每 5 秒刷新一次设备状态</span><span>当前展示 {{ devices.length }} 台 / {{ stats?.devices ?? 0 }} 台设备</span></div>
  </section>
</template>
