<script setup lang="ts">
import { computed, inject, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { api, type Device, type Sample } from '../api/client'
import DeviceStatusBadge from '../components/DeviceStatusBadge.vue'
import TrendChart from '../components/TrendChart.vue'

const route = useRoute()
const siteId = inject('siteId', ref('plant-a'))
const deviceId = computed(() => decodeURIComponent(String(route.params.deviceId ?? '')))
const device = ref<Device | null>(null)
const samples = ref<Sample[]>([])
const latest = ref<Sample | null>(null)
const historyWindowMs = ref(15 * 60 * 1000)
const socketState = ref<'connecting' | 'live' | 'retrying' | 'closed'>('connecting')
const error = ref('')
const selectedMetric = ref('')
let socket: WebSocket | undefined
let retryTimer = 0
let retryCount = 0
let disposed = false
let socketGeneration = 0

function connect() {
  if (disposed) return
  const generation = ++socketGeneration
  socketState.value = retryCount ? 'retrying' : 'connecting'
    const url = new URL('/ws/v1/monitor', window.location.origin)
  url.protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  url.searchParams.set('site_id', siteId.value)
  url.searchParams.set('device_id', deviceId.value)
  socket = new WebSocket(url)
  socket.onopen = () => { socketState.value = 'live'; retryCount = 0 }
  socket.onmessage = (event) => {
    try {
      const message = JSON.parse(event.data) as { type?: string; seq?: number; timestamp_ms?: number; received_ms?: number; metrics?: Record<string, number> }
      if (message.type !== 'telemetry' || !message.metrics) return
      const sample: Sample = { seq: message.seq, timestamp_ms: message.timestamp_ms,
        received_ms: message.received_ms, metrics: message.metrics }
      latest.value = sample
      if (!samples.value.some((item) => item.seq === sample.seq)) samples.value.push(sample)
      const maximumSamples = historyWindowMs.value >= 24 * 60 * 60 * 1000 ? 4096 : 1000
      if (samples.value.length > maximumSamples)
        samples.value.splice(0, samples.value.length - maximumSamples)
    } catch { error.value = '收到无法解析的实时帧' }
  }
  socket.onerror = () => { socketState.value = 'retrying' }
  socket.onclose = () => {
    if (disposed || generation !== socketGeneration) return
    socketState.value = 'retrying'
    retryCount += 1
    retryTimer = window.setTimeout(connect, Math.min(1000 * 2 ** Math.min(retryCount, 5), 30000))
  }
}

async function load() {
  error.value = ''
  samples.value = []
  try {
    const [deviceResponse, latestResponse] = await Promise.all([
      api.device(deviceId.value, siteId.value),
      api.latest(deviceId.value, siteId.value),
    ])
    device.value = deviceResponse
    latest.value = latestResponse.latest
    selectedMetric.value = Object.keys(latest.value?.metrics ?? samples.value.at(-1)?.metrics ?? {})[0] ?? ''
    await loadHistory()
  } catch (reason) { error.value = reason instanceof Error ? reason.message : '设备数据读取失败' }
  if (latest.value && !samples.value.some((item) => item.seq === latest.value?.seq)) samples.value.push(latest.value)
  connect()
}

async function loadHistory() {
  const toMs = Date.now()
  const limit = historyWindowMs.value >= 24 * 60 * 60 * 1000 ? 4096 : 1000
  try {
    const history = await api.history(deviceId.value, siteId.value,
      toMs - historyWindowMs.value, toMs, limit)
    samples.value = history.samples.slice(-limit)
    if (latest.value && !samples.value.some((item) => item.seq === latest.value?.seq))
      samples.value.push(latest.value)
    if (history.aggregated) selectedMetric.value ||= Object.keys(latest.value?.metrics ?? {})[0] ?? ''
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : '历史趋势读取失败'
  }
}

const metricEntries = computed(() => Object.entries(latest.value?.metrics ?? {}))
watch([deviceId, siteId], () => { socketGeneration += 1; socket?.close(); window.clearTimeout(retryTimer); void load() })
watch(historyWindowMs, () => { if (device.value) void loadHistory() })
onMounted(() => void load())
onBeforeUnmount(() => { disposed = true; window.clearTimeout(retryTimer); socket?.close() })
</script>

<template>
  <section class="detail-intro panel"><div class="detail-device-mark">◈</div><div class="detail-device-title"><p class="section-kicker">{{ device?.model || 'INDUSTRIAL ENDPOINT' }} / {{ deviceId }}</p><h2>{{ device?.display_name || deviceId }}</h2><div class="detail-meta"><DeviceStatusBadge :status="device?.status ?? 'unknown'" /><span>站点 {{ siteId }}</span><span>最新序号 {{ latest?.seq ?? device?.last_seq ?? '—' }}</span></div></div><div class="live-connection" :class="`live-${socketState}`"><span></span>{{ { connecting: '连接中', live: '实时链路正常', retrying: '重连中', closed: '已断开' }[socketState] }}</div></section>
  <div v-if="error" class="error-banner" role="alert"><span>!</span>{{ error }}<button @click="load">重试</button></div>
  <section class="metric-grid detail-metrics"><article v-for="[name, value] in metricEntries" :key="name" class="metric-card metric-green"><div class="metric-head"><span>{{ name }}</span><span class="metric-index">LIVE VALUE</span></div><strong>{{ typeof value === 'number' ? value.toFixed(2) : value.avg.toFixed(2) }}<b>{{ name.endsWith('_c') ? '°C' : '' }}</b></strong><small>来自最近一次已提交遥测</small><span class="metric-watermark">⌁</span></article><article v-if="!metricEntries.length" class="metric-card"><div class="metric-head"><span>当前测点</span><span class="metric-index">NO DATA</span></div><strong>—</strong><small>设备首次上报后显示实时测点</small><span class="metric-watermark">◌</span></article></section>
  <section class="panel chart-panel"><div class="section-heading"><div><p class="section-kicker">{{ historyWindowMs >= 24 * 60 * 60 * 1000 ? 'MINUTE AGGREGATES' : 'RAW SAMPLES' }}</p><h2>测点变化趋势</h2></div><div class="chart-controls"><label class="history-window-control"><span>历史时间范围</span><select v-model.number="historyWindowMs" aria-label="历史时间范围"><option :value="15 * 60 * 1000">最近 15 分钟</option><option :value="24 * 60 * 60 * 1000">最近 24 小时 · 分钟聚合</option></select></label><div class="metric-tabs"><button v-for="name in metricEntries.map(([key]) => key)" :key="name" :class="{ selected: selectedMetric === name }" @click="selectedMetric = name">{{ name }}</button><div class="live-key"><span></span>实时</div></div></div></div><TrendChart :samples="samples" :metric-name="selectedMetric" /><div class="chart-foot"><span>时间轴按所选窗口展示；长窗口采用分钟聚合</span><span>样本 {{ samples.length.toLocaleString() }} 点</span></div></section>
  <section class="detail-footnote"><span>序号缺口</span><strong>{{ device?.sequence_gap_count ?? 0 }}</strong><span>服务端仅在数据库提交后向设备发送应用层确认。</span></section>
</template>
