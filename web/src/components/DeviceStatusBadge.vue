<script setup lang="ts">
defineProps<{ status: string }>()
const labels: Record<string, string> = {
  online: '在线', stale: '数据过期', never_seen: '未上报', disabled: '已禁用', unknown: '未知',
}
</script>

<template>
  <span class="status-badge" :class="`status-${status}`" :aria-label="`设备状态：${labels[status] ?? labels.unknown}`">
    <span class="status-dot"></span>{{ labels[status] ?? labels.unknown }}
  </span>
</template>
