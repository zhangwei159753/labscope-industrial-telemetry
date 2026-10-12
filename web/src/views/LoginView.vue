<script setup lang="ts">
import { ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { api } from '../api/client'
import { setUser } from '../auth/session'

const username = ref('')
const password = ref('')
const error = ref('')
const submitting = ref(false)
const route = useRoute()
const router = useRouter()

async function submit() {
  if (submitting.value) return
  submitting.value = true
  error.value = ''
  try {
    const session = await api.login(username.value.trim(), password.value)
    setUser(session.user)
    password.value = ''
    const redirect = route.query.redirect
    const destination = typeof redirect === 'string' && redirect.startsWith('/') && !redirect.startsWith('//')
      ? redirect : '/overview'
    await router.replace(destination)
  } catch (reason) {
    error.value = reason instanceof Error ? reason.message : '登录失败，请稍后重试'
  } finally {
    submitting.value = false
  }
}
</script>

<template>
  <main class="login-page">
    <div class="login-card panel">
      <RouterLink class="brand login-brand" to="/login" aria-label="LabScope">
        <span class="brand-symbol">L<span></span></span>
        <span class="brand-name">LAB<span>SCOPE</span><small>INDUSTRIAL TELEMETRY</small></span>
      </RouterLink>
      <p class="section-kicker">SECURE OPERATIONS CONSOLE</p>
      <h1>登录监控平台</h1>
      <p class="login-description">使用已发放的操作员账户查看授权站点的设备遥测。</p>
      <form @submit.prevent="submit">
        <label>用户名<input v-model="username" name="username" autocomplete="username" required maxlength="64" autofocus></label>
        <label>密码<input v-model="password" name="password" type="password" autocomplete="current-password" required maxlength="1024"></label>
        <p v-if="error" class="login-error" role="alert">{{ error }}</p>
        <button class="login-submit" type="submit" :disabled="submitting">{{ submitting ? '正在验证…' : '登录工作台' }}<span>↗</span></button>
      </form>
      <div class="login-foot"><span>HttpOnly 会话 · 服务端授权</span><span>只读遥测控制台</span></div>
    </div>
  </main>
</template>
