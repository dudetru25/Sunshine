<script setup>
import { computed, onMounted, ref } from 'vue'
import { apiFetch } from '../../fetch_utils'
import Checkbox from '../../Checkbox.vue'

const props = defineProps([
  'config',
])

const config = ref(props.config)
const modules = ref([])
const appStreamingStatus = ref(null)
const busyAction = ref('')

const appStreamingIssues = computed(() => appStreamingStatus.value?.issues ?? [])

async function refreshModules() {
  const response = await fetch('./api/modules')
  const body = await response.json()
  modules.value = body.modules ?? []

  const appStreaming = modules.value.find((module) => module.id === 'app_streaming')
  appStreamingStatus.value = appStreaming?.status_detail ?? null

  const statusResponse = await fetch('./api/modules/app_streaming')
  const statusBody = await statusResponse.json()
  appStreamingStatus.value = statusBody.module?.status_detail ?? appStreamingStatus.value
}

async function runAppStreamingAction(action) {
  busyAction.value = action
  try {
    const response = await apiFetch(`./api/modules/app_streaming/actions/${action}`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({}),
    })
    const body = await response.json()
    appStreamingStatus.value = body.app_streaming ?? appStreamingStatus.value
    await refreshModules()
  } finally {
    busyAction.value = ''
  }
}

onMounted(refreshModules)
</script>

<template>
  <div class="config-page">
    <fieldset class="border rounded p-3 mb-3">
      <legend class="float-none w-auto px-2 h6">{{ $t('config.modules_app_streaming') }}</legend>

      <div class="d-flex flex-wrap align-items-center gap-2 mb-3">
        <span class="badge" :class="appStreamingStatus?.provider_available ? 'text-bg-success' : 'text-bg-warning'">
          {{ appStreamingStatus?.provider || 'none' }}
        </span>
        <span class="badge" :class="config.app_streaming_enabled === 'enabled' ? 'text-bg-primary' : 'text-bg-secondary'">
          {{ config.app_streaming_enabled === 'enabled' ? $t('_common.enabled') : $t('_common.disabled') }}
        </span>
        <button class="btn btn-sm btn-outline-secondary" type="button" @click="refreshModules">
          {{ $t('config.modules_refresh') }}
        </button>
        <button class="btn btn-sm btn-outline-danger" type="button" :disabled="busyAction === 'cleanup'" @click="runAppStreamingAction('cleanup')">
          {{ $t('config.modules_cleanup') }}
        </button>
        <button class="btn btn-sm btn-outline-secondary" type="button" :disabled="busyAction === 'doctor'" @click="runAppStreamingAction('doctor')">
          {{ $t('config.modules_doctor') }}
        </button>
      </div>

      <div v-if="appStreamingIssues.length" class="alert alert-warning py-2">
        <div v-for="issue in appStreamingIssues" :key="issue">{{ issue }}</div>
      </div>

      <div class="row g-3">
        <div class="col-12">
          <Checkbox
            id="app_streaming_enabled"
            locale-prefix="config"
            v-model="config.app_streaming_enabled"
          />
        </div>

        <div class="col-md-6">
          <label for="app_streaming_provider" class="form-label">{{ $t('config.app_streaming_provider') }}</label>
          <select id="app_streaming_provider" class="form-select" v-model="config.app_streaming_provider">
            <option value="sudovda">SudoVDA</option>
          </select>
          <div class="form-text">{{ $t('config.app_streaming_provider_desc') }}</div>
        </div>

        <div class="col-md-6">
          <label for="app_streaming_default_resolution" class="form-label">{{ $t('config.app_streaming_default_resolution') }}</label>
          <input id="app_streaming_default_resolution" type="text" class="form-control" v-model="config.app_streaming_default_resolution" placeholder="client" />
          <div class="form-text">{{ $t('config.app_streaming_default_resolution_desc') }}</div>
        </div>

        <div class="col-md-6">
          <label for="app_streaming_default_client_display_mode" class="form-label">{{ $t('config.app_streaming_default_client_display_mode') }}</label>
          <select id="app_streaming_default_client_display_mode" class="form-select" v-model="config.app_streaming_default_client_display_mode">
            <option value="windowed">{{ $t('apps.app_streaming_client_windowed') }}</option>
            <option value="borderless">{{ $t('apps.app_streaming_client_borderless') }}</option>
            <option value="fullscreen">{{ $t('apps.app_streaming_client_fullscreen') }}</option>
          </select>
        </div>

        <div class="col-md-6">
          <label for="app_streaming_sudovda_device_name" class="form-label">{{ $t('config.app_streaming_sudovda_device_name') }}</label>
          <input id="app_streaming_sudovda_device_name" type="text" class="form-control" maxlength="13" v-model="config.app_streaming_sudovda_device_name" />
          <div class="form-text">{{ $t('config.app_streaming_sudovda_device_name_desc') }}</div>
        </div>

        <div class="col-md-6">
          <label for="app_streaming_sudovda_serial" class="form-label">{{ $t('config.app_streaming_sudovda_serial') }}</label>
          <input id="app_streaming_sudovda_serial" type="text" class="form-control" maxlength="13" v-model="config.app_streaming_sudovda_serial" />
          <div class="form-text">{{ $t('config.app_streaming_sudovda_serial_desc') }}</div>
        </div>

        <div class="col-md-6">
          <label for="app_streaming_window_timeout_ms" class="form-label">{{ $t('config.app_streaming_window_timeout_ms') }}</label>
          <input id="app_streaming_window_timeout_ms" type="number" class="form-control" min="0" v-model="config.app_streaming_window_timeout_ms" />
        </div>

        <div class="col-md-6">
          <label for="app_streaming_window_follow_timeout_ms" class="form-label">{{ $t('config.app_streaming_window_follow_timeout_ms') }}</label>
          <input id="app_streaming_window_follow_timeout_ms" type="number" class="form-control" min="0" v-model="config.app_streaming_window_follow_timeout_ms" />
        </div>

        <div class="col-12">
          <Checkbox id="app_streaming_startup_cleanup" locale-prefix="config" v-model="config.app_streaming_startup_cleanup" />
          <Checkbox id="app_streaming_default_client_app_window" locale-prefix="config" v-model="config.app_streaming_default_client_app_window" />
          <Checkbox id="app_streaming_default_client_absolute_mouse" locale-prefix="config" v-model="config.app_streaming_default_client_absolute_mouse" />
          <Checkbox id="app_streaming_default_show_cursor" locale-prefix="config" v-model="config.app_streaming_default_show_cursor" />
          <Checkbox id="app_streaming_default_terminate_on_disconnect" locale-prefix="config" v-model="config.app_streaming_default_terminate_on_disconnect" />
          <Checkbox id="app_streaming_follow_windows" locale-prefix="config" v-model="config.app_streaming_follow_windows" />
          <Checkbox id="app_streaming_borderless_windows" locale-prefix="config" v-model="config.app_streaming_borderless_windows" />
          <Checkbox id="app_streaming_discover_start_menu" locale-prefix="config" v-model="config.app_streaming_discover_start_menu" />
        </div>
      </div>
    </fieldset>
  </div>
</template>
