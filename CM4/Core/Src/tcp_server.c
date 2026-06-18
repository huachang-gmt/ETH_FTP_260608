#include "tcp_server.h"
#include "string.h"
#include "strings.h"
#include "lwip/tcp.h"
#include "main.h"

static struct tcp_pcb *server_pcb = NULL;
static struct tcp_pcb *pasv_pcb = NULL;      // 常駐，不重複 bind
static struct tcp_pcb *data_client_pcb = NULL;
static struct tcp_pcb *control_client_pcb = NULL; 

static char ftp_rx_buffer[256];
static uint16_t ftp_rx_index = 0;
static uint8_t data_connected = 0;

typedef enum {
    FTP_CMD_NONE = 0,
    FTP_CMD_LIST,
    FTP_CMD_NLST,
    FTP_CMD_RETR
} ftp_cmd_t;

static ftp_cmd_t pending_cmd = FTP_CMD_NONE;

static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);      
static err_t pasv_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);                 
static err_t ftp_data_sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len);

// 新增：當資料確定傳送成功並收到 ACK 後，lwIP 會自動呼叫這個函式
static err_t ftp_data_sent_callback(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    // 進入此處代表 FileZilla 已成功收到我們發過去的目錄或檔案資料
    
    // 清除該 PCB 的所有回呼，防止重複觸發
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    
    if (tpcb == data_client_pcb)
    {
        data_client_pcb = NULL;
        data_connected = 0;
    }

    // 安全關閉資料通道 (資料已確認送達，此時關閉非常安全)
    tcp_close(tpcb);

    // 資料安全傳完並關閉後，向「控制通道」發送 226 狀態碼
    if(control_client_pcb != NULL)
    {
        const char *msg226 = "226 Transfer complete\r\n";
        tcp_write(control_client_pcb, msg226, strlen(msg226), TCP_WRITE_FLAG_COPY);
        tcp_output(control_client_pcb);
    }

    return ERR_OK;
}

static void send_dir_list(void)
{
    if(data_client_pcb != NULL)
    {
        const char *dir_data =
            "-rw-r--r-- 1 root root 123 test.txt\r\n"
            "-rw-r--r-- 1 root root 456 log.txt\r\n"; 

        // 註冊 sent 回呼，告訴 lwIP：當對方收到這條資料後，請呼叫 ftp_data_sent_callback
        tcp_sent(data_client_pcb, ftp_data_sent_callback);

        tcp_write(data_client_pcb, dir_data, strlen(dir_data), TCP_WRITE_FLAG_COPY);
        tcp_output(data_client_pcb);
        
        // 注意：這裡絕對不能呼叫 tcp_close() 或 tcp_abort()！關閉動作移到了 sent 裡面
    }
}

static void send_test_file(void)
{
    if(data_client_pcb != NULL)
    {
        const char *file_data = "Hello STM32 FTP Server\r\n";

        // 註冊 sent 回呼
        tcp_sent(data_client_pcb, ftp_data_sent_callback);

        tcp_write(data_client_pcb, file_data, strlen(file_data), TCP_WRITE_FLAG_COPY);
        tcp_output(data_client_pcb);
        
        // 同樣不在此處關閉，靜候 lwIP 非同步傳送完成
    }
}

void tcp_server_init(void)
{
    ip_addr_t ipaddr;
    IP_ADDR4(&ipaddr, 0, 0, 0, 0);

    // 1. 初始化 Port 21 控制通道監聽
    server_pcb = tcp_new();
    if(server_pcb != NULL) {
        if(tcp_bind(server_pcb, &ipaddr, 21) == ERR_OK) {
            server_pcb = tcp_listen(server_pcb);
            tcp_accept(server_pcb, tcp_accept_callback);
        }
    }

    // 2. 初始化 Port 2020 資料通道監聽（常駐，永不重複關閉與綁定）
    pasv_pcb = tcp_new();
    if(pasv_pcb != NULL) {
        if(tcp_bind(pasv_pcb, &ipaddr, 2020) == ERR_OK) {
            pasv_pcb = tcp_listen(pasv_pcb);
            tcp_accept(pasv_pcb, pasv_accept_callback);
        }
    }
}

static err_t tcp_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{   
    control_client_pcb = newpcb; 
    tcp_recv(newpcb, tcp_recv_callback);                  

    const char *msg = "220 STM32 FTP Server Ready\r\n";
    tcp_write(newpcb, msg, strlen(msg), TCP_WRITE_FLAG_COPY);
    tcp_output(newpcb);

    return ERR_OK;
}

static err_t tcp_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    if(p == NULL)
    {
        if(tpcb == control_client_pcb) control_client_pcb = NULL;
        ftp_rx_index = 0;
        tcp_close(tpcb);
        return ERR_OK;
    }

    char *rx_data = (char *)p->payload;
    for(uint16_t i = 0; i < p->tot_len; i++)
    {
        if(ftp_rx_index < sizeof(ftp_rx_buffer)-1)
        {
            ftp_rx_buffer[ftp_rx_index++] = rx_data[i];
        }

        if(rx_data[i] == '\n')
        {
            ftp_rx_buffer[ftp_rx_index] = 0;
            const char *reply = NULL; 

            if(strncmp(ftp_rx_buffer, "USER", 4) == 0) reply = "331 Password required\r\n";
            else if(strncmp(ftp_rx_buffer, "PASS", 4) == 0) reply = "230 Login successful\r\n";
            else if(strncmp(ftp_rx_buffer, "SYST", 4) == 0) reply = "215 UNIX Type: L8\r\n";
            else if(strncmp(ftp_rx_buffer, "FEAT", 4) == 0)
            {
                const char *feat_reply = "211-Features\r\n UTF8\r\n PASV\r\n211 End\r\n";
                tcp_write(tpcb, feat_reply, strlen(feat_reply), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);
                reply = NULL;
            }
            else if(strncmp(ftp_rx_buffer, "TYPE", 4) == 0) reply = "200 Type set OK\r\n";
            else if(strncmp(ftp_rx_buffer, "XPWD", 4) == 0 || strncmp(ftp_rx_buffer, "PWD", 3) == 0) reply = "257 \"/\" is current directory\r\n";
            else if(strncmp(ftp_rx_buffer, "QUIT", 4) == 0) reply = "221 Goodbye\r\n";
            else if(strncmp(ftp_rx_buffer, "NOOP", 4) == 0) reply = "200 OK\r\n";
            else if(strncmp(ftp_rx_buffer, "OPTS", 4) == 0) reply = "200 UTF8 enabled\r\n";
            else if(strncmp(ftp_rx_buffer, "PASV", 4) == 0)
            {
                // 因為 pasv_pcb 已常駐監聽 Port 2020，這裡直接回覆即可
                reply = "227 Entering Passive Mode (192,168,88,10,7,228)\r\n";
            }
            else if(strncmp(ftp_rx_buffer, "PORT", 4) == 0) reply = "200 PORT command successful\r\n";
            else if(strncmp(ftp_rx_buffer, "LIST", 4) == 0 || strncmp(ftp_rx_buffer, "NLST", 4) == 0)
            {
                const char *msg150 = "150 Opening ASCII mode data connection for file list\r\n";
                tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);

                if(data_connected == 1) send_dir_list();
                else pending_cmd = (strncmp(ftp_rx_buffer, "LIST", 4) == 0) ? FTP_CMD_LIST : FTP_CMD_NLST;
                reply = NULL; 
            }
            else if(strncmp(ftp_rx_buffer, "RETR", 4) == 0)
            {
                const char *msg150 = "150 Opening data connection\r\n";
                tcp_write(tpcb, msg150, strlen(msg150), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);

                if(data_connected == 1) send_test_file();
                else pending_cmd = FTP_CMD_RETR;
                reply = NULL;
            }
            else if(strncmp(ftp_rx_buffer, "CWD", 3) == 0) reply = "250 Directory changed\r\n";
            else if(strncmp(ftp_rx_buffer, "CDUP", 4) == 0) reply = "200 Directory changed\r\n";
            else if(strncmp(ftp_rx_buffer, "SIZE", 4) == 0) reply = "213 24\r\n";
            else if(strncmp(ftp_rx_buffer, "MDTM", 4) == 0) reply = "213 20260609000000\r\n";
            else if(strncmp(ftp_rx_buffer, "REST", 4) == 0) reply = "350 Restart position accepted\r\n";
            else if(strncmp(ftp_rx_buffer, "STRU", 4) == 0) reply = "200 STRU F OK\r\n";
            else if(strncmp(ftp_rx_buffer, "MODE", 4) == 0) reply = "200 MODE S OK\r\n";
            else if(strncmp(ftp_rx_buffer, "STAT", 4) == 0) reply = "211 FTP Server OK\r\n";
            else if(strncmp(ftp_rx_buffer, "DELE", 4) == 0) reply = "250 File deleted\r\n";
            else if(strncmp(ftp_rx_buffer, "RNFR", 4) == 0) reply = "350 Ready for RNTO\r\n";
            else if(strncmp(ftp_rx_buffer, "RNTO", 4) == 0) reply = "250 Rename successful\r\n";
            else if(strncmp(ftp_rx_buffer, "EPSV", 4) == 0 || strncmp(ftp_rx_buffer, "EPRT", 4) == 0) reply = "500 Not supported\r\n";
            else
            {
                char debug_buf[300];
                snprintf(debug_buf, sizeof(debug_buf), "500 DEBUG:%s", ftp_rx_buffer);
                tcp_write(tpcb, debug_buf, strlen(debug_buf), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);

                ftp_rx_index = 0;
                tcp_recved(tpcb, p->tot_len);
                pbuf_free(p);
                return ERR_OK;
            }

            if(reply != NULL)
            {
                tcp_write(tpcb, reply, strlen(reply), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);
            }
            ftp_rx_index = 0;
        }
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t pasv_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    data_client_pcb = newpcb;
    data_connected = 1;

    // 保持 pasv_pcb 連續監聽，不對它進行任何 close 動作

    if(pending_cmd == FTP_CMD_LIST || pending_cmd == FTP_CMD_NLST)
    {
        send_dir_list();        
        pending_cmd = FTP_CMD_NONE; 
    }    
    else if(pending_cmd == FTP_CMD_RETR)
    {
        send_test_file();
        pending_cmd = FTP_CMD_NONE; 
    }

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET); 
    return ERR_OK;
}