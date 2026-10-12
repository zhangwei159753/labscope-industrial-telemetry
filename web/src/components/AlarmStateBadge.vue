<script setup lang="ts">
defineProps<{ state: string; severity?: string }>()
const stateLabels: Record<string, string> = { active: '活动', acknowledged: '已确认', recovered: '已恢复' }
const severityLabels: Record<string, string> = { info: '提示', warning: '警告', critical: '严重' }
</script>

<template>
  <span class="alarm-state-badge" :class="[`alarm-${state}`, `severity-${severity ?? 'info'}`]"
    :aria-label="`告警状态：${stateLabels[state] ?? '未知'}，级别：${severityLabels[severity ?? 'info'] ?? '未知'}`">
    <i></i>{{ stateLabels[state] ?? '未知' }}<small>{{ severityLabels[severity ?? 'info'] ?? '未知' }}</small>
  </span>
</template>
