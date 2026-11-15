/*
 * NTC thermistor in a voltage divider, read by an A/D converter.
 *
 * The thermistor follows the beta model, R(T) = R25 * exp(B * (1/T -
 * 1/298.15 K)), and forms a divider with a fixed resistor between the
 * converter reference and ground.  With "low-side" (the default) the
 * thermistor is the leg to ground, so the input falls as it warms:
 *
 *   code = 2^bits * R(T) / (R(T) + Rref)
 *
 * GPIO output 0 carries the conversion result for the current temperature;
 * connect it to an analog input of the converter.  The "temperature"
 * property (millidegrees Celsius) can be changed at run time, e.g.
 * "qom-set /machine/thermistor temperature 31500"; "adc" and "resistance"
 * read back the result.  The ntc_thermistor_update trace event reports
 * every change.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "hw/irq.h"
#include "hw/misc/ntc_thermistor.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "trace.h"

#define NTC_T25_KELVIN 298.15
#define NTC_TEMP_MIN (-55000)       /* millidegrees Celsius */
#define NTC_TEMP_MAX 150000

static double ntc_resistance(NTCThermistorState *s)
{
    double t = s->temperature / 1000.0 + 273.15;

    return s->r25 * exp(s->beta * (1.0 / t - 1.0 / NTC_T25_KELVIN));
}

static uint32_t ntc_code(NTCThermistorState *s)
{
    double r = ntc_resistance(s);
    double full = 1u << s->adc_bits;
    double frac = (s->low_side ? r : s->r_ref) / (r + s->r_ref);

    return MIN(lround(frac * full), (long)full - 1);
}

static void ntc_update(NTCThermistorState *s)
{
    s->code = ntc_code(s);
    trace_ntc_thermistor_update(s->description ? s->description : "",
                                s->temperature, lround(ntc_resistance(s)),
                                s->code);
    qemu_set_irq(s->out, s->code);
}

static void ntc_get_temperature(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    NTCThermistorState *s = NTC_THERMISTOR(obj);

    visit_type_int32(v, name, &s->temperature, errp);
}

static void ntc_set_temperature(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    NTCThermistorState *s = NTC_THERMISTOR(obj);
    int32_t value;

    if (!visit_type_int32(v, name, &value, errp)) {
        return;
    }
    if (value < NTC_TEMP_MIN || value > NTC_TEMP_MAX) {
        error_setg(errp, "temperature %d m°C out of range (%d to %d)",
                   value, NTC_TEMP_MIN, NTC_TEMP_MAX);
        return;
    }
    s->temperature = value;
    if (DEVICE(obj)->realized) {
        ntc_update(s);
    }
}

static void ntc_get_resistance(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    uint32_t ohms = lround(ntc_resistance(NTC_THERMISTOR(obj)));

    visit_type_uint32(v, name, &ohms, errp);
}

static void ntc_realize(DeviceState *dev, Error **errp)
{
    NTCThermistorState *s = NTC_THERMISTOR(dev);

    if (!s->r25 || !s->beta || !s->r_ref) {
        error_setg(errp, "r25, beta and r-ref must be non-zero");
        return;
    }
    if (s->adc_bits < 8 || s->adc_bits > 16) {
        error_setg(errp, "adc-bits must be 8 to 16");
        return;
    }
    ntc_update(s);
}

static void ntc_init(Object *obj)
{
    NTCThermistorState *s = NTC_THERMISTOR(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->out, 1);

    s->temperature = 25000;
    object_property_add(obj, "temperature", "int32",
                        ntc_get_temperature, ntc_set_temperature,
                        NULL, NULL);
    object_property_set_description(obj, "temperature",
        "Temperature in millidegrees Celsius");
    object_property_add_uint32_ptr(obj, "adc", &s->code,
                                   OBJ_PROP_FLAG_READ);
    object_property_set_description(obj, "adc",
        "Conversion result driven on the output");
    object_property_add(obj, "resistance", "uint32",
                        ntc_get_resistance, NULL, NULL, NULL);
    object_property_set_description(obj, "resistance",
        "Thermistor resistance in ohms");
}

static int ntc_post_load(void *opaque, int version_id)
{
    NTCThermistorState *s = opaque;

    s->code = ntc_code(s);
    return 0;
}

static const VMStateDescription vmstate_ntc_thermistor = {
    .name = TYPE_NTC_THERMISTOR,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ntc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(temperature, NTCThermistorState),
        VMSTATE_END_OF_LIST()
    }
};

static Property ntc_properties[] = {
    DEFINE_PROP_STRING("description", NTCThermistorState, description),
    DEFINE_PROP_UINT32("r25", NTCThermistorState, r25, 10000),
    DEFINE_PROP_UINT32("beta", NTCThermistorState, beta, 3950),
    DEFINE_PROP_UINT32("r-ref", NTCThermistorState, r_ref, 10000),
    DEFINE_PROP_BOOL("low-side", NTCThermistorState, low_side, true),
    DEFINE_PROP_UINT8("adc-bits", NTCThermistorState, adc_bits, 10),
    DEFINE_PROP_END_OF_LIST(),
};

static void ntc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "NTC thermistor voltage divider";
    dc->realize = ntc_realize;
    dc->vmsd = &vmstate_ntc_thermistor;
    device_class_set_props(dc, ntc_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ntc_thermistor_info = {
    .name = TYPE_NTC_THERMISTOR,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(NTCThermistorState),
    .instance_init = ntc_init,
    .class_init = ntc_class_init,
};

static void ntc_thermistor_register_types(void)
{
    type_register_static(&ntc_thermistor_info);
}

type_init(ntc_thermistor_register_types)

NTCThermistorState *ntc_thermistor_create_simple(Object *parent,
                                                 const char *name,
                                                 const char *description,
                                                 uint32_t r25, uint32_t beta,
                                                 uint32_t r_ref,
                                                 qemu_irq adc_in)
{
    DeviceState *dev = qdev_new(TYPE_NTC_THERMISTOR);

    qdev_prop_set_string(dev, "description", description);
    qdev_prop_set_uint32(dev, "r25", r25);
    qdev_prop_set_uint32(dev, "beta", beta);
    qdev_prop_set_uint32(dev, "r-ref", r_ref);
    qdev_connect_gpio_out(dev, 0, adc_in);
    object_property_add_child(parent, name, OBJECT(dev));
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    return NTC_THERMISTOR(dev);
}
