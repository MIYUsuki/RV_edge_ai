# RV_edge_ai
## 项目简介
基于RV1103与ESP32S3的多模态边缘AI智能监控系统  
支持：
- 本地目标检测
- 实时视频监控
- 微信小程序远程查看
- MQTT消息推送
- 边缘计算隐私保护
## 系统架构
<img width="431" height="181" alt="project_structure drawio" src="https://github.com/user-attachments/assets/01a60ea6-d08a-4f2a-9cb9-3477687f65ea" />    


如图所示,项目通过SC3336、海凌科HLK LD2410B毫米波雷达和红外传感器等外设  
在RV1103平台上实现画面采集与捕获，并通过GPIO握手实现uart传输跌倒检测结果的稳定性  
画面传输目前在RV1103端使用tcp推流至PC实现，目的是方便调试  
ESP32S3则通过uart接收RV1103推理结果并与毫米波雷达传入数据进行联合决策，并将结果经由MQTT协议发送至broker  
小程序端则通过MQTT broker的消息提醒使用者监控范围内出现目标事件  
## 数据流图DFD  
<img width="489" height="421" alt="Image" src="https://github.com/user-attachments/assets/9d4a2a66-d60b-4188-b372-ee2fd1d906cd" />  


如图示  
摄像头采集画面之后通过RGA和MMAP实现零拷贝推理
输出置信度结果至esp32s3端  
esp32s3配合接收并解析了的毫米波雷达能量值进行融合判断  
同时红外会在未探查到目标时让esp进入ULP(Ultra Low Power)以降低功耗
