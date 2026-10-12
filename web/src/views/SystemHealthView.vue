<script setup lang="ts">
import { inject, onBeforeUnmount, onMounted, ref } from 'vue'
import { api, type Stats } from '../api/client'

const siteId = inject('siteId', ref('plant-a'))
const stats = ref<Stats | null>(null)
const system = ref<Record<string, unknown> | null>(null)
const apiState = ref<'checking' | 'online' | 'offline'>('checking')
const error = ref('')
const checkedAt = ref('—')
let refreshTimer = 0
async function refresh() {
  try {
    const [, nextStats] = await Promise.all([api.health(), api.stats(siteId.value)])
    stats.value = nextStats
    apiState.value = 'online'
    error.value = ''
    try { system.value = await api.getSystemHealth() } catch { system.value = null }
  } catch (reason) {
    apiState.value = 'offline'
    error.value = reason instanceof Error ? reason.message : '健康检查失败'
  }
  checkedAt.value = new Date().toLocaleTimeString('zh-CN', { hour12: false })
}
onMounted(() => { void refresh(); refreshTimer = window.setInterval(() => void refresh(), 10000) })
onBeforeUnmount(() => window.clearInterval(refreshTimer))
</script>

<template>
  <div v-if="error" class="error-banner" role="alert"><span>!</span>{{ error }}<button @click="refresh">重试</button></div>
  <section class="system-grid">
    <article class="panel system-summary"><p class="section-kicker">SERVICE PROBE / HTTP</p><div class="system-status" :class="`probe-${apiState}`"><span></span><strong>{{ { checking: '检测中', online: '服务可用', offline: '服务不可用' }[apiState] }}</strong></div><p>来自 Drogon `/healthz` 健康探针；它只代表 HTTP 进程已响应。</p><small>最近检查 {{ checkedAt }}</small></article>
    <article class="panel system-summary"><p class="section-kicker">INGESTION / LAST 60 SEC</p><div class="system-number">{{ stats?.samples_last_minute?.toLocaleString() ?? '—' }}<small>条</small></div><p>最近 60 秒内接收并提交的遥测样本。</p><small>站点范围 {{ siteId }} · 当前设备 {{ stats?.devices ?? '—' }} 台</small></article>
  </section>
  <section class="panel status-list-panel"><div class="section-heading"><div><p class="section-kicker">OPERATIONAL BOUNDARY</p><h2>运行状态说明</h2></div><span class="local-mode-pill">单机 Compose · 开发环境</span></div>
    <div class="status-list"><div><span class="status-list-icon icon-green">✓</span><div><strong>HTTP 接口</strong><small>已检查 /healthz 与统计 API</small></div><b :class="`state-${apiState}`">{{ apiState === 'online' ? '可用' : apiState === 'checking' ? '检查中' : '不可用' }}</b></div>
      <div><span class="status-list-icon" :class="system?.database === 'healthy' ? 'icon-green' : ''">↗</span><div><strong>PostgreSQL</strong><small>持久化与监控查询依赖</small></div><b :class="system?.database === 'healthy' ? '' : 'state-unknown'">{{ system?.database === 'healthy' ? '可用' : system?.database === 'unavailable' ? '不可用' : '未检查' }}</b></div>
      <div><span class="status-list-icon" :class="system?.mqtt === 'connected' ? 'icon-green' : ''">⌁</span><div><strong>MQTT Broker</strong><small>设备遥测订阅连接</small></div><b :class="system?.mqtt === 'connected' ? '' : 'state-unknown'">{{ system?.mqtt === 'connected' ? '已连接' : system?.mqtt === 'disconnected' ? '已断开' : '未检查' }}</b></div>
      <div><span class="status-list-icon" :class="system?.alarm_processor === 'running' ? 'icon-green' : ''">!</span><div><strong>告警处理器</strong><small>数据库 outbox 规则评估线程</small></div><b :class="system?.alarm_processor === 'running' ? '' : 'state-unknown'">{{ system?.alarm_processor === 'running' ? '运行中' : system?.alarm_processor === 'stopped' ? '已停止' : '未检查' }}</b></div>
      <div><span class="status-list-icon">▤</span><div><strong>遥测写入队列</strong><small>容量 {{ Number(system?.ingestion_queue_capacity ?? 0).toLocaleString() }}</small></div><b>{{ Number(system?.ingestion_queue_depth ?? 0).toLocaleString() }} 条</b></div>
      <div><span class="status-list-icon">◉</span><div><strong>实时监控连接</strong><small>当前已鉴权 WebSocket 订阅</small></div><b>{{ Number(system?.websocket_clients ?? 0) }} 个</b></div>
      <div><span class="status-list-icon">◷</span><div><strong>数据保留维护</strong><small>按小时调度分区清理任务</small></div><b>{{ system?.retention_maintenance === 'scheduled_hourly' ? '已调度' : '未检查' }}</b></div>
      <div><span class="status-list-icon">◇</span><div><strong>身份与权限</strong><small>当前会话由服务端校验；站点范围由用户授权关系决定</small></div><b class="state-unknown">会话鉴权</b></div>
    </div>
    <div class="system-note"><strong>运行边界</strong><span>/healthz 仅表示 HTTP 进程可响应；依赖健康通过受保护的系统接口读取。Prometheus 指标不包含设备 ID 等高基数字段。</span></div>
  </section>
</template>
