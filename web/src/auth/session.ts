import { readonly, ref } from 'vue'
import type { AuthUser } from '../api/client'

const currentUser = ref<AuthUser | null>(null)
export const user = readonly(currentUser)

export function setUser(value: AuthUser | null) {
  currentUser.value = value
}
