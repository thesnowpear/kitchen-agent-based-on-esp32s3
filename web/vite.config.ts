import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Vite 开发服务配置：管理面板需要通过 localhost 使用 Web Serial API。
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    strictPort: false,
  },
});
