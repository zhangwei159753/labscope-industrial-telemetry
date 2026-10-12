<script setup lang="ts">
import { computed, inject, ref, watch } from 'vue'
import { api, type AlarmRule } from '../api/client'
import { user } from '../auth/session'

const siteId = inject('siteId', ref(''))
const rules = ref<AlarmRule[]>([])
const loading = ref(false)
const saving = ref(false)
const error = ref('')
const notice = ref('')
const canManage = computed(() => user.value?.role === 'admin')
const form = ref({ device_id: '', metric: '', direction: 'above' as 'above' | 'below', threshold: 80,
  duration_ms: 0, hysteresis: 0, cooldown_ms: 0, severity: 'warning' as 'info' | 'warning' | 'critical' })

async function load() {
  if (!siteId.value) { rules.value = []; return }
  loading.value = true
  error.value = ''
  try { rules.value = (await api.listAlarmRules({ siteId: siteId.value, limit: 100 })).rules }
  catch (reason) { error.value = reason instanceof Error ? reason.message : '规则列表读取失败' }
  finally { loading.value = false }
}

async function createRule() {
  if (!siteId.value || !canManage.value) return
  saving.value = true
  error.value = ''
  notice.value = ''
  try {
    await api.createAlarmRule({ site_id: siteId.value, ...form.value, enabled: true })
    notice.value = '规则已创建并写入操作审计。'
    form.value = { device_id: '', metric: '', direction: 'above', threshold: 80,
      duration_ms: 0, hysteresis: 0, cooldown_ms: 0, severity: 'warning' }
    await load()
  } catch (reason) { error.value = reason instanceof Error ? reason.message : '规则创建失败' }
  finally { saving.value = false }
}

async function setEnabled(rule: AlarmRule) {
  error.value = ''
  try {
    await api.updateAlarmRule(rule.id, !rule.enabled)
    notice.value = `规则 ${rule.id} 已${rule.enabled ? '停用' : '启用'}。`
    await load()
  } catch (reason) { error.value = reason instanceof Error ? reason.message : '规则状态更新失败' }
}

watch(siteId, () => void load(), { immediate: true })
</script>

<template>
  <div v-if="error" class="error-banner" role="alert"><span>!</span>{{ error }}<button type="button" @click="load">重试</button></div>
  <div v-if="notice" class="success-banner" role="status">{{ notice }}</div>
  <section v-if="canManage" class="panel rule-create-panel">
    <div class="section-heading"><div><p class="section-kicker">RULE CONFIGURATION</p><h2>新增测点规则</h2></div><span class="local-mode-pill">管理员 · 所有修改留痕</span></div>
    <form class="rule-form" @submit.prevent="createRule">
      <label>设备编号<input v-model.trim="form.device_id" aria-label="设备编号" required maxlength="128" placeholder="pump-01"></label>
      <label>测点名称<input v-model.trim="form.metric" aria-label="测点名称" required maxlength="128" placeholder="temperature_c"></label>
      <label>判断方向<select v-model="form.direction"><option value="above">高于阈值</option><option value="below">低于阈值</option></select></label>
      <label>阈值<input v-model.number="form.threshold" aria-label="阈值" type="number" step="any" required></label>
      <label>持续时间 (ms)<input v-model.number="form.duration_ms" type="number" min="0" max="86400000"></label>
      <label>恢复回差<input v-model.number="form.hysteresis" type="number" min="0" step="any"></label>
      <label>冷却时间 (ms)<input v-model.number="form.cooldown_ms" type="number" min="0" max="604800000"></label>
      <label>告警级别<select v-model="form.severity"><option value="info">提示</option><option value="warning">警告</option><option value="critical">严重</option></select></label>
      <button type="submit" :disabled="saving">{{ saving ? '提交中…' : '创建规则' }}</button>
    </form>
  </section>
  <section class="panel alarm-panel rules-panel">
    <div class="section-heading"><div><p class="section-kicker">PERSISTED RULES</p><h2>规则台账</h2></div><span class="local-mode-pill">站点 {{ siteId }} · 最多 100 条</span></div>
    <div v-if="loading && !rules.length" class="alarm-empty" role="status">正在读取规则…</div>
    <div v-else-if="!rules.length" class="alarm-empty"><span>⌁</span><strong>该站点还没有规则</strong><small>创建规则后，已提交遥测将按序进入评估队列</small></div>
    <article v-for="rule in rules" :key="rule.id" class="rule-row">
      <div><strong>{{ rule.metric }} · {{ rule.device_id }}</strong><small>{{ rule.direction === 'above' ? '高于' : '低于' }} {{ rule.threshold }} · 持续 {{ rule.duration_ms }} ms · 回差 {{ rule.hysteresis }}</small></div>
      <span class="rule-severity" :class="`severity-${rule.severity}`">{{ { info: '提示', warning: '警告', critical: '严重' }[rule.severity] }}</span>
      <span :class="rule.enabled ? 'rule-enabled' : 'rule-disabled'">{{ rule.enabled ? '运行中' : '已停用' }}</span>
      <button v-if="canManage" type="button" :aria-label="`${rule.enabled ? '停用' : '启用'}规则 ${rule.id}`" @click="setEnabled(rule)">{{ rule.enabled ? '停用' : '启用' }}</button>
    </article>
    <p v-if="!canManage && rules.length" class="read-only-note">当前角色可以查看规则，只有管理员可以创建或停用规则。</p>
  </section>
</template>
