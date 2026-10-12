<script setup lang="ts">
import { inject, onMounted, ref } from 'vue'
import { RouterLink } from 'vue-router'
import { api, type Device } from '../api/client'
import DeviceStatusBadge from '../components/DeviceStatusBadge.vue'

const siteId = inject('siteId', ref('plant-a'))
const devices = ref<Device[]>([])
const cursor = ref<string | null>(null)
const search = ref('')
const status = ref('')
const loading = ref(false)
const error = ref('')
async function load(reset = true) {
  loading.value = true
  try {
    if (reset) { devices.value = []; cursor.value = null }
    const page = await api.devices({ siteId: siteId.value, status: status.value || undefined,
      cursor: reset ? undefined : cursor.value, limit: 50 })
    devices.value = reset ? page.devices : [...devices.value, ...page.devices]
    cursor.value = page.next_cursor
    error.value = ''
  } catch (reason) { error.value = reason instanceof Error ? reason.message : '设备列表读取失败' }
  finally { loading.value = false }
}
const filtered = () => devices.value.filter((device) =>
  `${device.display_name} ${device.device_id} ${device.model}`.toLowerCase().includes(search.value.toLowerCase()))
onMounted(() => void load())
</script>

<template>
  <section class="panel fleet-panel devices-view">
    <div class="section-heading"><div><p class="section-kicker">REGISTERED ENDPOINTS</p><h2>设备台账 <small>{{ devices.length }} 台</small></h2></div><div class="registry-tag"><span class="inline-dot"></span>站点范围：{{ siteId }}</div></div>
    <div class="filter-bar"><label class="search-box"><span>⌕</span><input v-model="search" placeholder="搜索设备名称、编号或型号" aria-label="搜索设备"></label>
      <label class="filter-select"><span>状态筛选</span><select v-model="status" @change="load()"><option value="">全部状态</option><option value="online">在线</option><option value="stale">数据过期</option><option value="never_seen">未上报</option><option value="disabled">已禁用</option></select></label>
      <button class="secondary-button" :disabled="loading" @click="load()">{{ loading ? '读取中…' : '刷新列表 ↻' }}</button></div>
    <div v-if="error" class="error-banner" role="alert"><span>!</span>{{ error }}</div>
    <div class="table-scroll"><table class="device-table"><thead><tr><th>设备 / DEVICE</th><th>型号</th><th>状态</th><th>最后上报</th><th>最新序号</th><th>序号缺口</th><th></th></tr></thead><tbody>
      <tr v-for="device in filtered()" :key="`${device.site_id}/${device.device_id}`"><td><RouterLink class="device-name" :to="`/devices/${encodeURIComponent(device.device_id)}?site_id=${encodeURIComponent(siteId)}`"><span class="device-glyph">◈</span><span><strong>{{ device.display_name || device.device_id }}</strong><small>{{ device.device_id }}</small></span></RouterLink></td><td class="muted-cell">{{ device.model || '通用遥测' }}</td><td><DeviceStatusBadge :status="device.status" /></td><td class="mono-cell">{{ device.last_seen_at ? new Date(device.last_seen_at).toLocaleString('zh-CN', { hour12: false }) : '—' }}</td><td class="mono-cell">{{ device.last_seq }}</td><td class="mono-cell">{{ device.sequence_gap_count }}</td><td class="row-arrow">↗</td></tr>
      <tr v-if="!filtered().length"><td colspan="7"><div class="empty-row"><span>◌</span><strong>{{ loading ? '正在读取' : search ? '没有匹配设备' : '暂时没有设备' }}</strong><small>可以更换站点、筛选条件或先注册设备</small></div></td></tr>
    </tbody></table></div>
    <div class="table-foot"><span><i class="foot-dot"></i>使用设备游标分页，单页最多 50 条</span><button v-if="cursor" class="text-link button-link" :disabled="loading" @click="load(false)">载入更多 <span>↓</span></button><span v-else>已显示当前筛选的全部结果</span></div>
  </section>
</template>
