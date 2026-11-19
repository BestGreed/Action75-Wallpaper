# 按键播放规则

在 settings.json 的 groups 加入下列组。roles 指向本组控件 ID，选项的 toggleId 指向一个布尔控件。0 代表组合，正整数代表候选。order 完整列出作者开放的每个正整数一次。宿主据此绘制勾选、顺序、随机开关和击键间隔。事件到来时才计数，渲染过程无需每帧计算规则。

```json
{"id":"bursts","label":"可播放的图案","presentation":"variant-playback",
 "playback":{"selection":"choice","randomize":"random","interval":"keys","order":[2,1]},
 "controls":[
  {"id":"choice","label":"图案","type":"select","default":0,"options":[{"value":0,"label":"组合"},{"value":1,"label":"第一种","toggleId":"first"},{"value":2,"label":"第二种","toggleId":"second"}]},
  {"id":"first","label":"第一种","type":"toggle","default":true},
  {"id":"second","label":"第二种","type":"toggle","default":true},
  {"id":"random","label":"随机播放","type":"toggle","default":false},
  {"id":"keys","label":"击键间隔","type":"slider","default":3,"min":1,"max":99,"step":1}
 ]}
```

播放角色由组关联，可以省略上述角色控件的 binding。普通外观控件仍使用有效绑定。新版支持此协议的 Effekseer 播放器中，1 对应 manifest.effects 的第一个作者文件，2 对应第二个；烟火专用后端中，数值对应宿主花型。测试时固定两个候选、关闭随机、每三键播放，记录九次真实触发回执以区分“画面有变化”和“顺序确实正确”。旧发行版的多效果粒子播放能力要按实际版本验证。
