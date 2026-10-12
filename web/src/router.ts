import { createRouter, createWebHistory } from 'vue-router'
import { api } from './api/client'
import { setUser, user } from './auth/session'

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', redirect: '/overview' },
    { path: '/login', component: () => import('./views/LoginView.vue'), meta: { public: true } },
    { path: '/overview', component: () => import('./views/OverviewView.vue'), meta: { title: '运行总览', eyebrow: 'FLEET OVERVIEW' } },
    { path: '/devices', component: () => import('./views/DevicesView.vue'), meta: { title: '设备台账', eyebrow: 'DEVICE REGISTRY' } },
    { path: '/devices/:deviceId', component: () => import('./views/DeviceDetailView.vue'), meta: { title: '设备实时监控', eyebrow: 'LIVE DEVICE DETAIL' } },
    { path: '/alarms', component: () => import('./views/AlarmsView.vue'), meta: { title: '告警中心', eyebrow: 'ALARM OPERATIONS' } },
    { path: '/alarm-rules', component: () => import('./views/AlarmRulesView.vue'), meta: { title: '告警规则', eyebrow: 'RULE CONFIGURATION' } },
    { path: '/system', component: () => import('./views/SystemHealthView.vue'), meta: { title: '系统健康', eyebrow: 'SERVICE STATUS' } },
  ],
})

router.beforeEach(async (to) => {
  if (to.meta.public) {
    if (user.value) return '/overview'
    return true
  }
  if (!user.value) {
    try {
      const session = await api.me()
      setUser(session.user)
    } catch {
      setUser(null)
      return { path: '/login', query: { redirect: to.fullPath } }
    }
  }
  return true
})

export default router
