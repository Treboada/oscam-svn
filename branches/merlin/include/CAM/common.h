#ifndef __CAM_COMMON_H__
#  define __CAM_COMMON_H__

#  define SCT_LEN(sct) (3+((sct[1]&0x0f)<<8)+sct[2])

int cam_common_detect(uchar *, ushort);
int cam_common_load_card();

int cam_common_process_ecm(ECM_REQUEST *);
int cam_common_process_emm(EMM_PACKET *);

int cam_common_cmd2card(uchar *, ushort, uchar *, ushort, ushort *);

ulong cam_common_get_provider_id(uchar *, ushort);
void cam_common_guess_card_system(ECM_REQUEST *);

#endif // __CAM_COMMON_H__
