<script setup lang="ts">
import { computed, inject, ref, watch } from 'vue'
import { ApiError, api, type AlertEvent } from '../api/client'
import { user } from '../auth/session'
import AlarmStateBadge from '../components/AlarmStateBadge.vue'

const siteId = inject('siteId', ref(''))
const deviceFilter = ref('')
const stateFilter = ref('')
const severityFilter = ref('')
const events = ref<AlertEvent[]>([])
const remarks = ref<Record<string, string>>({})
const loading = ref(false)
const error = ref('')
const notice = ref('')
const nextCursor = ref<string | null>(null)
const canAcknowledge = computed(() => user.value?.role === 'admin' || user.value?.role === 'operator')

async function load(append = false) {
  if (!siteId.value) { events.value = []; return }
  loading.value = true
  error.value = ''
  try {
    const result = await api.listAlerts({ siteId: siteId.value, deviceId: deviceFilter.value.trim() || undefined,
      state: stateFilter.value || undefined, severity: severityFilter.value || undefined,
      cursor: append ? nextCursor.value ?? undefined : undefined, limit: 50 })
    events.value = append ? [...events.value, ...result.events] : result.events
    nextCursor.value = result.next_cursor
  } catch (reason) {
    error.value = reason instanceof ApiError && reason.status === 403
      ? '当前账户无权查看该站点告警。' : reason instanceof Error ? reason.message : '告警列表读取失败'
  } finally { loading.value = false }
}

async function acknowledge(event: AlertEvent) {
  notice.value = ''
  error.value = ''
  try {
    await api.acknowledgeAlert(event.id, siteId.value, remarks.value[event.id]?.trim() ?? '')
    notice.value = `告警 ${event.id} 已确认，操作已记录。`
    await load()
  } catch (reason) {
    error.value = reason instanceof ApiError && reason.status === 403
      ? `服务端拒绝确认告警：${reason.message}` : reason instanceof Error ? reason.message : '告警确认失败'
  }
}

watch([siteId, deviceFilter, stateFilter, severityFilter], () => { void load() }, { immediate: true })
</script>

<template>
  <div v-if="error" class="error-banner" role="alert"><span>!</span>{{ error }}<button type="button" @click="load()">重试</button></div>
  <div v-if="notice" class="success-banner" role="status">{{ notice }}</div>
  <section class="alarm-summary-grid">
    <article class="panel alarm-summary"><span>当前筛选</span><strong>{{ events.length.toLocaleString() }}</strong><small>最多 50 条 / 页</small></article>
    <article class="panel alarm-summary"><span>活动告警</span><strong>{{ events.filter((event) => event.state === 'active').length }}</strong><small>需要值班人员处理</small></article>
    <article class="panel alarm-summary"><span>处置权限</span><strong>{{ canAcknowledge ? '可确认' : '只读' }}</strong><small>{{ user?.role ?? '未登录' }} · 服务端再次鉴权</small></article>
  </section>
  <section class="panel alarm-panel">
    <div class="section-heading"><div><p class="section-kicker">ALARM EVENT STREAM</p><h2>告警事件</h2></div><span class="local-mode-pill">提交后确认 · 留存审计</span></div>
    <div class="alarm-filters">
      <label>设备<input v-model="deviceFilter" aria-label="设备筛选" placeholder="按设备编号筛选"></label>
      <label>状态<select v-model="stateFilter" aria-label="告警状态"><option value="">全部状态</option><option value="active">活动</option><option value="acknowledged">已确认</option><option value="recovered">已恢复</option></select></label>
      <label>级别<select v-model="severityFilter" aria-label="告警级别"><option value="">全部级别</option><option value="critical">严重</option><option value="warning">警告</option><option value="info">提示</option></select></label>
    </div>
    <div v-if="loading && !events.length" class="alarm-empty" role="status">正在读取告警…</div>
    <div v-else-if="!events.length" class="alarm-empty"><span>◇</span><strong>当前条件下没有告警</strong><small>新事件提交后会显示在这里</small></div>
    <article v-for="event in events" :key="event.id" class="alarm-event" :data-event-id="event.id">
      <div class="alarm-event-mark">!</div>
      <div class="alarm-event-main"><div class="alarm-event-heading"><strong>{{ event.metric }} · {{ event.device_id }}</strong><AlarmStateBadge :state="event.state" :severity="event.severity" /></div>
        <p>触发值 <b>{{ Number(event.value ?? 0).toFixed(2) }}</b><span>规则 #{{ event.rule_id ?? '—' }}</span></p>
        <small>触发时间 {{ new Date(event.triggered_at).toLocaleString('zh-CN', { hour12: false }) }} · 序号 {{ event.seq ?? '—' }}</small>
        <p v-if="event.acknowledgment_remark" class="alarm-remark">处置备注：{{ event.acknowledgment_remark }}</p>
        <div v-if="canAcknowledge && event.state === 'active'" class="alarm-ack-control">
          <input v-model="remarks[event.id]" aria-label="确认备注" maxlength="512" placeholder="填写处置说明（可选）">
          <button type="button" :aria-label="`确认告警 ${event.id}`" @click="acknowledge(event)">确认并留痕</button>
        </div>
      </div>
    </article>
    <div v-if="nextCursor" class="alarm-more"><button type="button" :disabled="loading" @click="load(true)">加载更多</button></div>
  </section>
</template>
