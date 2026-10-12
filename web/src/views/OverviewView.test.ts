// @vitest-environment jsdom
import { flushPromises, mount } from '@vue/test-utils'
import { createMemoryHistory, createRouter } from 'vue-router'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'
import OverviewView from './OverviewView.vue'
import DevicesView from './DevicesView.vue'
import DeviceDetailView from './DeviceDetailView.vue'
import DeviceStatusBadge from '../components/DeviceStatusBadge.vue'

vi.mock('../api/client', () => ({
  api: {
    stats: vi.fn(), devices: vi.fn(), device: vi.fn(), latest: vi.fn(), history: vi.fn(),
  },
}))

import { api } from '../api/client'

const pump = {
  site_id: 'plant-a', device_id: 'pump-1', display_name: '冷却泵 1', model: 'P-100',
  enabled: true, last_seq: 7, sequence_gap_count: 0, status: 'online', last_seen_at: null,
}

async function mountWithRoute(component: unknown, path: string) {
  const router = createRouter({
    history: createMemoryHistory(),
    routes: [{ path: '/devices/:deviceId', component: DeviceDetailView }],
  })
  await router.push(path)
  await router.isReady()
  const wrapper = mount(component as never, {
    global: { plugins: [router], provide: { siteId: ref('plant-a') }, stubs: { TrendChart: true } },
  })
  return wrapper
}

class FakeSocket {
  static instances: FakeSocket[] = []
  onopen: (() => void) | null = null
  onmessage: ((event: { data: string }) => void) | null = null
  onerror: (() => void) | null = null
  onclose: (() => void) | null = null
  constructor(public readonly url: string | URL) { FakeSocket.instances.push(this) }
  close() { this.onclose?.() }
}

describe('operations monitoring views', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    FakeSocket.instances = []
    vi.stubGlobal('WebSocket', FakeSocket)
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} })
    vi.useFakeTimers()
    vi.setSystemTime(new Date('2026-10-11T08:00:00Z'))
  })

  it.each([
    ['online', '在线'], ['stale', '数据过期'], ['never_seen', '未上报'],
    ['disabled', '已禁用'], ['unknown', '未知'],
  ])('renders the accessible %s state and overview empty state', async (status, label) => {
    vi.mocked(api.stats).mockResolvedValue({ devices: 0, enabled_devices: 0,
      online_devices: 0, stale_devices: 0, samples_last_minute: 0 })
    vi.mocked(api.devices).mockResolvedValue({ devices: [], next_cursor: null })
    const badge = mount(DeviceStatusBadge, { props: { status } })
    expect(badge.get(`[aria-label="设备状态：${label}"]`).text()).toContain(label)
    const wrapper = await mountWithRoute(OverviewView, '/devices/pump-1')
    await flushPromises()
    expect(wrapper.text()).toContain('当前站点还没有设备')
    expect(wrapper.text()).toContain('接入设备')
    wrapper.unmount()
  })

  it('filters the loaded device page and requests subsequent pages by cursor', async () => {
    vi.mocked(api.devices)
      .mockResolvedValueOnce({ devices: [pump], next_cursor: 'plant-a/pump-1' })
      .mockResolvedValueOnce({ devices: [{ ...pump, device_id: 'pump-2', status: 'stale' }], next_cursor: null })
    const wrapper = await mountWithRoute(DevicesView, '/devices/pump-1')
    await flushPromises()
    expect(wrapper.text()).toContain('冷却泵 1')
    await wrapper.get('input[aria-label="搜索设备"]').setValue('pump-2')
    expect(wrapper.text()).toContain('没有匹配设备')
    await wrapper.get('button.button-link').trigger('click')
    await flushPromises()
    expect(api.devices).toHaveBeenLastCalledWith({ siteId: 'plant-a', status: undefined,
      cursor: 'plant-a/pump-1', limit: 50 })
    expect(wrapper.text()).toContain('pump-2')
    wrapper.unmount()
  })

  it('loads a selected history window, displays live values, and reconnects after disconnect', async () => {
    vi.mocked(api.device).mockResolvedValue(pump)
    vi.mocked(api.latest).mockResolvedValue({ site_id: 'plant-a', device_id: 'pump-1',
      latest: { seq: 7, timestamp_ms: 1791705500000, metrics: { temperature_c: 42 } } })
    vi.mocked(api.history).mockResolvedValue({ samples: [], aggregated: false, has_more: false })
    const wrapper = await mountWithRoute(DeviceDetailView, '/devices/pump-1?site_id=plant-a')
    await flushPromises()
    expect(wrapper.text()).toContain('42.00')
    const historySelect = wrapper.find('select[aria-label="历史时间范围"]')
    expect(historySelect.exists()).toBe(true)
    await historySelect.setValue('900000')
    await flushPromises()
    expect(api.history).toHaveBeenLastCalledWith('pump-1', 'plant-a',
      Date.now() - 15 * 60 * 1000, Date.now(), 1000)
    const socket = FakeSocket.instances[0]
    socket.onopen?.()
    await flushPromises()
    expect(wrapper.text()).toContain('实时链路正常')
    socket.onmessage?.({ data: JSON.stringify({ type: 'telemetry', seq: 8,
      timestamp_ms: Date.now(), metrics: { temperature_c: 43 } }) })
    await flushPromises()
    expect(wrapper.text()).toContain('43.00')
    socket.onclose?.()
    await vi.advanceTimersByTimeAsync(2000)
    expect(FakeSocket.instances.length).toBeGreaterThan(1)
    wrapper.unmount()
    vi.useRealTimers()
  })
})
