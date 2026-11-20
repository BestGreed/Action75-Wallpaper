# Windows 壁纸播放器

[播放器入口](src/main.cpp) 负责输入和屏幕状态，使用 D3D11 输出画面。原生壁纸由独立 Worker 执行。[断屏重连测试](tests/monitor_reconnect.py) 覆盖短暂断开、释放渲染资源及重连后的首次按键。
