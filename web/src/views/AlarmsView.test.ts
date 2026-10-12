// @vitest-environment jsdom
import { flushPromises, mount } from '@vue/test-utils'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'
import AlarmsView from './AlarmsView.vue'
import AlarmRulesView from './AlarmRulesView.vue'
import { ApiError, type AlertEvent } from '../api/client'
import { setUser } from '../auth/session'

vi.mock('../api/client', () => ({
  ApiError: class ApiError extends Error { constructor(public status: number, message: string) { super(message) } },
  api: {
    listAlerts: vi.fn(), acknowledgeAlert: vi.fn(), listAlarmRules: vi.fn(),
    createAlarmRule: vi.fn(), updateAlarmRule: vi.fn(),
  },
}))

import { api } from '../api/client'

const events: AlertEvent[] = [
  { id: '17', rule_id: '4', site_id: 'plant-a', device_id: 'pump-1', metric: 'temperature_c', severity: 'critical', state: 'active', value: 91.2, triggered_at: '2026-10-11T08:00:00Z' },
  { id: '16', rule_id: '3', site_id: 'plant-a', device_id: 'pump-2', metric: 'pressure_kpa', severity: 'warning', state: 'acknowledged', value: 140, triggered_at: '2026-10-11T07:00:00Z', acknowledged_by: 'operator1', acknowledgment_remark: '已安排巡检' },
  { id: '15', rule_id: '2', site_id: 'plant-a', device_id: 'pump-3', metric: 'flow_lpm', severity: 'info', state: 'recovered', value: 10, triggered_at: '2026-10-11T06:00:00Z', recovered_at: '2026-10-11T06:05:00Z' },
]

function asRole(role: 'admin' | 'operator' | 'viewer') {
  setUser({ id: 8, username: 'operator1', role })
}

describe('alarm operations views', () => {
  beforeEach(() => {
    vi.clearAllMocks()
    asRole('operator')
    vi.mocked(api.listAlerts).mockResolvedValue({ events, next_cursor: null })
    vi.mocked(api.listAlarmRules).mockResolvedValue({ rules: [], next_cursor: null })
  })

  it('shows all event states and applies site, device, state, and severity filters', async () => {
    const wrapper = mount(AlarmsView, { global: { provide: { siteId: ref('plant-a') } } })
    await flushPromises()
    for (const label of ['活动', '已确认', '已恢复']) expect(wrapper.text()).toContain(label)
    await wrapper.get('input[aria-label="设备筛选"]').setValue('pump-2')
    await wrapper.get('select[aria-label="告警状态"]').setValue('acknowledged')
    await wrapper.get('select[aria-label="告警级别"]').setValue('warning')
    await flushPromises()
    expect(api.listAlerts).toHaveBeenLastCalledWith({ siteId: 'plant-a', deviceId: 'pump-2', state: 'acknowledged', severity: 'warning', limit: 50 })
    wrapper.unmount()
  })

  it('allows an operator to acknowledge an active event with an audit remark', async () => {
    vi.mocked(api.acknowledgeAlert).mockResolvedValue({ ok: true })
    const wrapper = mount(AlarmsView, { global: { provide: { siteId: ref('plant-a') } } })
    await flushPromises()
    const firstEvent = wrapper.get('[data-event-id="17"]')
    await firstEvent.get('input[aria-label="确认备注"]').setValue('已切换备用泵并通知值班长')
    await firstEvent.get('button[aria-label="确认告警 17"]').trigger('click')
    await flushPromises()
    expect(api.acknowledgeAlert).toHaveBeenCalledWith('17', 'plant-a', '已切换备用泵并通知值班长')
    expect(api.listAlerts).toHaveBeenCalledTimes(2)
    wrapper.unmount()
  })

  it('keeps the viewer read-only and presents server authorization failures', async () => {
    asRole('viewer')
    const wrapper = mount(AlarmsView, { global: { provide: { siteId: ref('plant-a') } } })
    await flushPromises()
    expect(wrapper.find('button[aria-label="确认告警 17"]').exists()).toBe(false)
    wrapper.unmount()

    asRole('operator')
    vi.mocked(api.acknowledgeAlert).mockRejectedValue(new ApiError(403, 'operator role required'))
    const operatorView = mount(AlarmsView, { global: { provide: { siteId: ref('plant-a') } } })
    await flushPromises()
    await operatorView.get('[data-event-id="17"] button[aria-label="确认告警 17"]').trigger('click')
    await flushPromises()
    expect(operatorView.get('[role="alert"]').text()).toContain('operator role required')
    operatorView.unmount()
  })

  it('lists rules and creates a bounded threshold rule for administrators', async () => {
    asRole('admin')
    vi.mocked(api.listAlarmRules).mockResolvedValue({ rules: [
      { id: '4', site_id: 'plant-a', device_id: 'pump-1', metric: 'temperature_c', direction: 'above', threshold: 85, duration_ms: 5000, hysteresis: 3, cooldown_ms: 60000, severity: 'critical', enabled: true },
    ], next_cursor: null })
    vi.mocked(api.createAlarmRule).mockResolvedValue({ rule_id: '5' })
    const wrapper = mount(AlarmRulesView, { global: { provide: { siteId: ref('plant-a') } } })
    await flushPromises()
    expect(wrapper.text()).toContain('temperature_c')
    await wrapper.get('input[aria-label="设备编号"]').setValue('pump-1')
    await wrapper.get('input[aria-label="测点名称"]').setValue('pressure_kpa')
    await wrapper.get('input[aria-label="阈值"]').setValue('120')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(api.createAlarmRule).toHaveBeenCalledWith(expect.objectContaining({ site_id: 'plant-a', device_id: 'pump-1', metric: 'pressure_kpa', threshold: 120 }))
    wrapper.unmount()
  })
})
