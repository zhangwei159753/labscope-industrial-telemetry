import { afterEach, describe, expect, it, vi } from 'vitest'
import { api, ApiError } from './client'

afterEach(() => vi.unstubAllGlobals())

describe('same-origin telemetry API client', () => {
  it('parses stats and sends browser cookies with same-origin requests', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({
      devices: 8, enabled_devices: 8, online_devices: 7, stale_devices: 1,
      samples_last_minute: 420, ingest_messages_per_second: 7,
    }), { status: 200, headers: { 'content-type': 'application/json' } }))
    vi.stubGlobal('fetch', fetchMock)
    const result = await api.stats('plant-a')
    expect(result.online_devices).toBe(7)
    expect(fetchMock).toHaveBeenCalledWith('/api/v1/stats?site_id=plant-a', expect.objectContaining({
      credentials: 'same-origin',
    }))
  })

  it('URL-encodes site and cursor filters for bounded device pages', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({ devices: [], next_cursor: null }), {
      status: 200, headers: { 'content-type': 'application/json' },
    }))
    vi.stubGlobal('fetch', fetchMock)
    await api.devices({ siteId: 'plant east', status: 'stale', cursor: 'plant east/motor 7', limit: 50 })
    expect(fetchMock.mock.calls[0][0]).toBe('/api/v1/devices?site_id=plant+east&status=stale&cursor=plant+east%2Fmotor+7&limit=50')
  })

  it('surfaces server errors without losing the HTTP status fallback', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({ error: 'device not found' }), {
      status: 404, headers: { 'content-type': 'application/json' },
    }))
    vi.stubGlobal('fetch', fetchMock)
    await expect(api.latest('unknown-device', 'plant-a')).rejects.toThrow('device not found')
  })

  it('logs in without persisting secrets and sends the session CSRF token on logout', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(new Response(JSON.stringify({
        user: { id: 7, username: 'operator-1', role: 'operator' }, csrf_token: 'csrf-session-token',
      }), { status: 200, headers: { 'content-type': 'application/json' } }))
      .mockResolvedValueOnce(new Response(JSON.stringify({ ok: true }), {
        status: 200, headers: { 'content-type': 'application/json' },
      }))
    vi.stubGlobal('fetch', fetchMock)
    const storageSpy = vi.fn()
    vi.stubGlobal('localStorage', { length: 0, getItem: vi.fn(), setItem: storageSpy, removeItem: vi.fn() })

    const session = await api.login('operator-1', 'secret-password')
    expect(session.user.role).toBe('operator')
    expect(fetchMock.mock.calls[0][0]).toBe('/api/v1/auth/login')
    const loginInit = fetchMock.mock.calls[0][1] as RequestInit
    expect(loginInit).toMatchObject({
      method: 'POST', credentials: 'same-origin',
      body: JSON.stringify({ username: 'operator-1', password: 'secret-password' }),
    })
    expect(new Headers(loginInit.headers).get('Content-Type')).toBe('application/json')
    await api.logout()
    const logoutInit = fetchMock.mock.calls[1][1] as RequestInit
    expect(logoutInit.method).toBe('POST')
    expect(new Headers(logoutInit.headers).get('X-CSRF-Token')).toBe('csrf-session-token')
    expect(storageSpy).not.toHaveBeenCalled()
  })

  it.each([400, 401, 403, 503])('maps HTTP %i into a typed API error', async (status) => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({ error: 'request denied' }), {
      status, headers: { 'content-type': 'application/json' },
    }))
    vi.stubGlobal('fetch', fetchMock)
    await expect(api.me()).rejects.toMatchObject({ status, message: 'request denied' } satisfies Partial<ApiError>)
  })

  it('URL-encodes alert filters and uses CSRF on a mutation', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(new Response(JSON.stringify({
        user: { id: 8, username: 'viewer', role: 'viewer' }, csrf_token: 'csrf-session-token',
      }), { status: 200, headers: { 'content-type': 'application/json' } }))
      .mockResolvedValueOnce(new Response(JSON.stringify({ events: [], next_cursor: null }), {
        status: 200, headers: { 'content-type': 'application/json' },
      }))
      .mockResolvedValueOnce(new Response(JSON.stringify({ ok: true }), {
        status: 200, headers: { 'content-type': 'application/json' },
      }))
      .mockResolvedValueOnce(new Response(JSON.stringify({ ok: true }), {
        status: 200, headers: { 'content-type': 'application/json' },
      }))
    vi.stubGlobal('fetch', fetchMock)
    await api.login('viewer', 'pw')
    await api.listAlerts({ siteId: 'plant east', deviceId: 'motor/7', cursor: 'event 4', limit: 25 })
    expect(fetchMock.mock.calls[1][0]).toBe('/api/v1/alarms/events?site_id=plant+east&device_id=motor%2F7&cursor=event+4&limit=25')
    await api.acknowledgeAlert('event/4', 'plant east', 'inspected')
    expect(fetchMock.mock.calls[2][0]).toBe('/api/v1/alarms/events/event%2F4/ack?site_id=plant+east')
    const ackInit = fetchMock.mock.calls[2][1] as RequestInit
    expect(ackInit.method).toBe('POST')
    expect(new Headers(ackInit.headers).get('X-CSRF-Token')).toBe('csrf-session-token')
  })

  it('loads the authenticated users allowed site list', async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({ sites: [
      { site_id: 'plant-a', display_name: 'Plant A' },
    ] }), { status: 200, headers: { 'content-type': 'application/json' } }))
    vi.stubGlobal('fetch', fetchMock)
    const result = await api.sites()
    expect(result.sites[0].site_id).toBe('plant-a')
    expect(fetchMock.mock.calls[0][0]).toBe('/api/v1/sites')
    expect(fetchMock.mock.calls[0][1]).toMatchObject({ credentials: 'same-origin' })
  })
})
