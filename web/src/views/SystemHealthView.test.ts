// @vitest-environment jsdom
import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'
import SystemHealthView from './SystemHealthView.vue'

vi.mock('../api/client', () => ({
  api: { health: vi.fn(), stats: vi.fn(), getSystemHealth: vi.fn() },
}))

import { api } from '../api/client'

describe('system health view', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    vi.mocked(api.health).mockResolvedValue('ok')
    vi.mocked(api.stats).mockResolvedValue({ devices: 18, enabled_devices: 18,
      online_devices: 17, stale_devices: 1, samples_last_minute: 1000 })
    vi.mocked(api.getSystemHealth).mockResolvedValue({ status: 'degraded', database: 'healthy', alarm_processor: 'running',
      mqtt: 'disconnected', ingestion_queue_depth: 24, ingestion_queue_capacity: 8192,
      websocket_clients: 3, retention_maintenance: 'scheduled_hourly' })
  })
  afterEach(() => vi.useRealTimers())

  it('distinguishes process health from dependency health and displays runtime gauges', async () => {
    vi.useFakeTimers()
    const wrapper = mount(SystemHealthView, { global: { provide: { siteId: ref('plant-a') } } })
    await flushPromises()
    expect(wrapper.text()).toContain('服务可用')
    expect(wrapper.text()).toContain('PostgreSQL')
    expect(wrapper.text()).toContain('MQTT Broker')
    expect(wrapper.text()).toContain('告警处理器')
    expect(wrapper.text()).toContain('运行中')
    expect(wrapper.text()).toContain('已断开')
    expect(wrapper.text()).toContain('24 条')
    expect(wrapper.text()).toContain('3 个')
    wrapper.unmount()
  })
})
