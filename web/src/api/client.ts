export interface Device {
  site_id: string
  device_id: string
  display_name: string
  model: string
  enabled: boolean
  last_seq: number
  sequence_gap_count: number
  status: 'online' | 'stale' | 'never_seen' | 'disabled' | string
  last_seen_at: string | null
}

export interface Stats {
  devices: number
  enabled_devices: number
  online_devices: number
  stale_devices: number
  samples_last_minute: number
  ingest_messages_per_second?: number
}

export interface Sample {
  seq?: number
  timestamp_ms?: number
  received_ms?: number
  device_time_ms?: number
  bucket_start_ms?: number
  metrics: Record<string, number | { min: number; max: number; avg: number; count: number }>
}

export interface DevicePage { devices: Device[]; next_cursor: string | null }
export interface LatestResponse { latest: Sample | null; site_id: string; device_id: string }
export interface HistoryResponse { samples: Sample[]; aggregated: boolean; has_more: boolean }
export type UserRole = 'admin' | 'operator' | 'viewer'
export interface AuthUser { id: number; username: string; role: UserRole }
export interface AuthSession { user: AuthUser; csrf_token: string }
export interface AlertEvent {
  id: string
  rule_id?: string
  site_id: string
  device_id: string
  metric: string
  severity: 'info' | 'warning' | 'critical'
  state: 'active' | 'acknowledged' | 'recovered'
  value?: number
  message?: string
  triggered_at: string
  seq?: number
  acknowledgment_remark?: string | null
  recovered_at?: string | null
  acknowledged_by?: string
  acknowledged_at?: string
}
export interface AlertPage { events: AlertEvent[]; next_cursor: string | null }
export interface AlarmRule {
  id: string
  site_id: string
  device_id: string
  metric: string
  direction: 'above' | 'below'
  threshold: number
  duration_ms: number
  hysteresis: number
  cooldown_ms: number
  severity: 'info' | 'warning' | 'critical'
  enabled: boolean
}
export interface AlarmRulePage { rules: AlarmRule[]; next_cursor: string | null }
export interface Site { site_id: string; display_name: string; timezone: string; enabled: boolean }
export interface SiteList { sites: Site[] }

export class ApiError extends Error {
  constructor(public readonly status: number, message: string) {
    super(message)
    this.name = 'ApiError'
  }
}

let csrfToken: string | undefined

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const method = init?.method?.toUpperCase() ?? 'GET'
  const headers = new Headers({ Accept: 'application/json' })
  new Headers(init?.headers).forEach((value, key) => headers.set(key, value))
  if (method !== 'GET' && method !== 'HEAD' && csrfToken) headers.set('X-CSRF-Token', csrfToken)
  const response = await fetch(path, {
    ...init,
    credentials: 'same-origin',
    headers,
  })
  if (!response.ok) {
    let message = `请求失败 (${response.status})`
    try {
      const body = await response.json() as { error?: string }
      if (body.error) message = body.error
    } catch { /* keep HTTP status fallback */ }
    throw new ApiError(response.status, message)
  }
  if (response.status === 204) return undefined as T
  return response.json() as Promise<T>
}

function jsonPost<T>(path: string, body?: unknown) {
  return request<T>(path, {
    method: 'POST',
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  })
}

function query(values: Record<string, string | number | undefined>) {
  const params = new URLSearchParams()
  for (const [key, value] of Object.entries(values)) {
    if (value !== undefined && value !== '') params.set(key, String(value))
  }
  return params.toString()
}

export const api = {
  login: async (username: string, password: string) => {
    const session = await jsonPost<AuthSession>('/api/v1/auth/login', { username, password })
    csrfToken = session.csrf_token
    return session
  },
  logout: async () => {
    try { return await jsonPost<{ ok: boolean }>('/api/v1/auth/logout') }
    finally { csrfToken = undefined }
  },
  me: () => request<AuthSession>('/api/v1/auth/me').then((session) => {
    csrfToken = session.csrf_token
    return session
  }),
  sites: () => request<SiteList>('/api/v1/sites'),
  overview: (siteId: string) => request<Stats & { active_alerts?: number; alarms_available?: boolean }>(
    `/api/v1/overview?${query({ site_id: siteId })}`),
  stats: (siteId?: string) => request<Stats>(`/api/v1/stats?${query({ site_id: siteId })}`),
  devices: (options: { siteId: string; status?: string; cursor?: string | null; limit?: number }) => {
    const params = query({ site_id: options.siteId, status: options.status,
      cursor: options.cursor ?? undefined, limit: options.limit ?? 50 })
    return request<DevicePage>(`/api/v1/devices?${params}`)
  },
  latest: (deviceId: string, siteId: string) => request<LatestResponse>(
    `/api/v1/devices/${encodeURIComponent(deviceId)}/latest?${query({ site_id: siteId })}`),
  device: (deviceId: string, siteId: string) => request<Device>(
    `/api/v1/devices/${encodeURIComponent(deviceId)}?${query({ site_id: siteId })}`),
  history: (deviceId: string, siteId: string, fromMs: number, toMs: number, limit = 500) =>
    request<HistoryResponse>(`/api/v1/devices/${encodeURIComponent(deviceId)}/history?${query({
      site_id: siteId, from_ms: fromMs, to_ms: toMs, limit,
    })}`),
  listAlerts: (options: { siteId?: string; deviceId?: string; severity?: string;
    state?: string; cursor?: string; limit?: number }) => request<AlertPage>(
    `/api/v1/alarms/events?${query({ site_id: options.siteId, device_id: options.deviceId,
      severity: options.severity, state: options.state, cursor: options.cursor, limit: options.limit ?? 50 })}`),
  acknowledgeAlert: (eventId: string, siteId: string, remark: string) => jsonPost<{ ok: boolean }>(
    `/api/v1/alarms/events/${encodeURIComponent(eventId)}/ack?${query({ site_id: siteId })}`, { remark }),
  listAlarmRules: (options: { siteId?: string; cursor?: string; limit?: number } = {}) =>
    request<AlarmRulePage>(`/api/v1/alarms/rules?${query({ site_id: options.siteId,
      cursor: options.cursor, limit: options.limit ?? 100 })}`),
  createAlarmRule: (rule: Omit<AlarmRule, 'id'>) => jsonPost<{ rule_id: string }>(
    '/api/v1/alarms/rules', rule),
  updateAlarmRule: (ruleId: string, enabled: boolean) => request<{ ok: boolean }>(
    `/api/v1/alarms/rules/${encodeURIComponent(ruleId)}`, {
      method: 'PATCH', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled }),
    }),
  getSystemHealth: () => request<Record<string, unknown>>('/api/v1/system/health'),
  health: async () => {
    const response = await fetch('/healthz', { credentials: 'same-origin' })
    if (!response.ok) throw new Error(`服务探活失败 (${response.status})`)
    return response.text()
  },
}

export function socketUrl(path: string) {
  const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${scheme}//${window.location.host}${path}`
}
