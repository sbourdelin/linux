#ifndef _DT_BINDINGS_PINCTRL_STM32_H
#define _DT_BINDINGS_PINCTRL_STM32_H

#define STM32_PIN_NO(x) ((x) << 8)
#define STM32_GET_PIN_NO(x) ((x) >> 8)
#define STM32_GET_PIN_FUNC(x) ((x) & 0xff)

#define STM32_PIN_GPIO		0
#define STM32_PIN_AF(x)		((x) + 1)
#define STM32_PIN_ANALOG	(STM32_PIN_AF(15) + 1)

#endif /* _DT_BINDINGS_PINCTRL_STM32_H */
