<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'
import * as echarts from 'echarts/core'
import { LineChart } from 'echarts/charts'
import { GridComponent, TooltipComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
import type { Sample } from '../api/client'

echarts.use([LineChart, GridComponent, TooltipComponent, CanvasRenderer])

const props = defineProps<{ samples: Sample[]; metricName?: string }>()
const element = ref<HTMLDivElement | null>(null)
let chart: echarts.ECharts | undefined
let resizeObserver: ResizeObserver | undefined

function metricValue(sample: Sample): number | null {
  const values = Object.entries(sample.metrics ?? {})
  const selected = props.metricName
    ? values.find(([name]) => name === props.metricName)
    : values[0]
  if (!selected) return null
  const value = selected[1]
  return typeof value === 'number' ? value : value.avg
}

function render() {
  if (!chart) return
  const points = props.samples.flatMap((sample) => {
    const value = metricValue(sample)
    const timestamp = sample.timestamp_ms ?? sample.device_time_ms ?? sample.bucket_start_ms
    return value === null || timestamp === undefined ? [] : [[timestamp, value]]
  })
  chart.setOption({
    animation: false,
    grid: { left: 44, right: 18, top: 18, bottom: 34 },
    tooltip: { trigger: 'axis', backgroundColor: '#172326', borderColor: '#334448',
      textStyle: { color: '#e7efeb' }, valueFormatter: (value: unknown) => `${value} °C` },
    xAxis: { type: 'time', boundaryGap: false, axisLine: { lineStyle: { color: '#344347' } },
      axisLabel: { color: '#748589', fontFamily: 'IBM Plex Mono, monospace', fontSize: 10 },
      splitLine: { show: false } },
    yAxis: { type: 'value', scale: true, splitNumber: 4,
      axisLabel: { color: '#748589', fontFamily: 'IBM Plex Mono, monospace', fontSize: 10 },
      splitLine: { lineStyle: { color: '#263437', type: 'dashed' } } },
    series: [{ type: 'line', data: points, smooth: 0.24, showSymbol: false,
      lineStyle: { color: '#8bc39e', width: 2 },
      areaStyle: { color: new echarts.graphic.LinearGradient(0, 0, 0, 1,
        [{ offset: 0, color: 'rgba(139,195,158,.22)' }, { offset: 1, color: 'rgba(139,195,158,0)' }]) } }],
  }, true)
}

onMounted(() => {
  if (!element.value) return
  chart = echarts.init(element.value, undefined, { renderer: 'canvas' })
  resizeObserver = new ResizeObserver(() => chart?.resize())
  resizeObserver.observe(element.value)
  render()
})
watch(() => [props.samples, props.metricName], render, { deep: true })
onBeforeUnmount(() => { resizeObserver?.disconnect(); chart?.dispose() })
</script>

<template>
  <div class="chart-wrap">
    <div ref="element" class="trend-chart" role="img" aria-label="最近遥测趋势图"></div>
    <div v-if="!samples.length" class="chart-empty"><span>◌</span><strong>等待遥测样本</strong><small>设备完成首次上报后，测点趋势将在此显示</small></div>
  </div>
</template>
