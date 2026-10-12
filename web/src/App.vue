<script setup lang="ts">
import { computed, onBeforeUnmount, provide, ref, watch } from 'vue'
import { RouterLink, RouterView, useRoute, useRouter } from 'vue-router'
import { api, type Site } from './api/client'
import { setUser, user } from './auth/session'

const siteId = ref('')
const sites = ref<Site[]>([])
const siteLoadError = ref('')
provide('siteId', siteId)
const route = useRoute()
const router = useRouter()
const isLogin = computed(() => route.path === '/login')
const title = computed(() => String(route.meta.title ?? '运行总览'))
const eyebrow = computed(() => String(route.meta.eyebrow ?? 'FLEET OVERVIEW'))
const now = ref(new Date())
const clockTimer = window.setInterval(() => { now.value = new Date() }, 1000)
onBeforeUnmount(() => window.clearInterval(clockTimer))
const clock = computed(() => new Intl.DateTimeFormat('zh-CN', {
  hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
}).format(now.value))

watch(user, async (current) => {
  sites.value = []
  siteId.value = ''
  siteLoadError.value = ''
  if (!current) return
  try {
    const result = await api.sites()
    if (user.value !== current) return
    sites.value = result.sites
    siteId.value = result.sites[0]?.site_id ?? ''
    if (!result.sites.length) siteLoadError.value = '当前账户尚未分配可访问站点，请联系管理员。'
  } catch (reason) {
    if (user.value !== current) return
    siteLoadError.value = reason instanceof Error ? reason.message : '站点列表暂时不可用'
  }
}, { immediate: true })

async function logout() {
  try { await api.logout() } catch { /* The server session may already have expired. */ }
  setUser(null)
  sites.value = []
  siteId.value = ''
  await router.replace('/login')
}
</script>

<template>
  <RouterView v-if="isLogin" />
  <div v-else class="application-shell">
    <aside class="sidebar">
      <RouterLink class="brand" to="/overview" aria-label="LabScope 运行总览">
        <span class="brand-symbol">L<span></span></span>
        <span class="brand-name">LAB<span>SCOPE</span><small>INDUSTRIAL TELEMETRY</small></span>
      </RouterLink>
      <div class="sidebar-section">工作台 <span>WORKSPACE</span></div>
      <nav class="primary-nav" aria-label="主导航">
        <RouterLink to="/overview"><span class="nav-icon">◫</span>运行总览<span class="nav-kicker">01</span></RouterLink>
        <RouterLink to="/devices"><span class="nav-icon">▦</span>设备台账<span class="nav-kicker">02</span></RouterLink>
        <RouterLink to="/alarms"><span class="nav-icon">⚑</span>告警中心<span class="nav-kicker">03</span></RouterLink>
        <RouterLink to="/alarm-rules"><span class="nav-icon">⚙</span>告警规则<span class="nav-kicker">04</span></RouterLink>
        <RouterLink to="/system"><span class="nav-icon">⌁</span>系统健康<span class="nav-kicker">05</span></RouterLink>
      </nav>
      <div class="sidebar-bottom">
        <div class="site-chip"><span class="site-pulse"></span><div><small>当前监测站点</small><strong>{{ siteId }}</strong></div><span class="site-chevron">⌄</span></div>
        <div class="sidebar-foot"><span class="foot-dot"></span>本地单机运行 <span>v0.1</span></div>
      </div>
    </aside>

    <main class="main-area">
      <header class="topbar">
        <div class="breadcrumb"><span>LABSCOPE</span><b>/</b>{{ title }}</div>
        <div class="topbar-right">
          <label class="site-select"><span>站点</span><select v-model="siteId" aria-label="当前监测站点" :disabled="sites.length === 0"><option v-for="site in sites" :key="site.site_id" :value="site.site_id">{{ site.display_name }} · {{ site.site_id }}</option></select></label>
          <div class="system-clock"><span class="clock-live"></span>系统时间 <strong>{{ clock }}</strong></div>
          <span class="user-chip"><strong>{{ user?.username }}</strong><small>{{ user?.role }}</small></span>
          <button class="logout-button" type="button" @click="logout">退出</button>
        </div>
      </header>

      <div class="page-content">
        <div v-if="siteLoadError" class="error-banner" role="alert"><span>!</span>{{ siteLoadError }}</div>
        <div class="page-heading">
          <div><p class="eyebrow">{{ eyebrow }} <span class="eyebrow-rule"></span> {{ siteId.toUpperCase() }}</p><h1>{{ title }}</h1></div>
          <div class="heading-tag"><span class="heading-square"></span>设备监控 · 权限控制</div>
        </div>
        <RouterView v-slot="{ Component }">
          <component :is="Component" :key="`${route.fullPath}:${siteId}`" />
        </RouterView>
      </div>
      <footer class="page-footer"><span>LABSCOPE INDUSTRIAL</span><span>遥测数据仅用于监测与分析，不提供设备控制</span><span>现场时钟 · {{ clock }}</span></footer>
    </main>
  </div>
</template>
