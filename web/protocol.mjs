export class LineDecoder {
  constructor(onEvent,onLog=()=>{}){this.buffer='';this.onEvent=onEvent;this.onLog=onLog;}
  push(text){this.buffer+=text; let i;while((i=this.buffer.indexOf('\n'))>=0){const line=this.buffer.slice(0,i).replace(/\x1b\[[0-9;]*m/g,'').trim();this.buffer=this.buffer.slice(i+1);const pos=line.indexOf('@bridge ');if(pos<0){if(line)this.onLog(line);continue;}try{const e=JSON.parse(line.slice(pos+8));if(e&&typeof e.event==='string')this.onEvent(e);}catch{this.onLog('无法解析设备响应：'+line);}}if(this.buffer.length>8192){this.buffer='';this.onLog('丢弃过长的串口行');}}
}
export const errors={scan_failed:'扫描启动失败',invalid_index:'设备编号无效',disconnect_first:'请先断开当前蓝牙设备',device_not_found:'设备已不可用，请重新扫描',clear_binding_first:'接收器只有一个绑定槽，请先清除旧绑定再选择新设备',clear_failed:'清除失败，请查看日志',busy:'接收器正忙，请稍后再试',command_too_long:'命令过长',unknown_command:'固件不支持此命令'};
